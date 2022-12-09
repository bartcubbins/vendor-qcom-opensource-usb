/*
 * Copyright (C) 2019-2021, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.usb@1.2-service-qti"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <assert.h>
#include <chrono>
#include <dirent.h>
#include <pthread.h>
#include <regex>
#include <stdio.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <cutils/uevent.h>
#include <hidl/HidlTransportSupport.h>
#include <linux/usb/ch9.h>
#include <sys/epoll.h>
#include <utils/Errors.h>
#include <utils/StrongPointer.h>

#include "Usb.h"

#define VENDOR_USB_ADB_DISABLED_PROP "vendor.sys.usb.adb.disabled"
#define USB_CONTROLLER_PROP "vendor.usb.controller"
#define USB_MODE_PATH "/sys/bus/platform/devices/"

namespace android {
namespace hardware {
namespace usb {
namespace V1_2 {
namespace implementation {

using ::android::base::Trim;
using ::android::base::ReadFileToString;
using ::android::base::WriteStringToFile;

const char GOOGLE_USB_VENDOR_ID_STR[] = "18d1";
const char GOOGLE_USBC_35_ADAPTER_UNPLUGGED_ID_STR[] = "5029";

// Set by the signal handler to destroy the thread
volatile bool destroyThread;

static bool checkUsbWakeupSupport();
static void checkUsbInHostMode();
static void checkUsbDeviceAutoSuspend(const std::string& devicePath);
static bool checkUsbInterfaceAutoSuspend(const std::string& devicePath,
        const std::string &intf);

static std::string appendRoleNodeHelper(const std::string &portName, PortRoleType type) {

    if ((portName == "..") || (portName.find('/') != std::string::npos)) {
       ALOGE("Fatal: invalid portName");
       return "";
    }

    std::string node("/sys/class/typec/" + portName);

    switch (type) {
      case PortRoleType::MODE: {
        std::string port_type(node + "/port_type");
        if (!access(port_type.c_str(), F_OK))
          return port_type;
        // port_type doesn't exist for UCSI. in that case fall back to data_role
      }
      //fall-through
      case PortRoleType::DATA_ROLE:
        return node + "/data_role";
      case PortRoleType::POWER_ROLE:
        return node + "/power_role";
      default:
        return "";
  }
}

static const char *convertRoletoString(PortRole role) {
  if (role.type == PortRoleType::POWER_ROLE) {
    if (role.role == static_cast<uint32_t>(PortPowerRole::SOURCE))
      return "source";
    else if (role.role == static_cast<uint32_t>(PortPowerRole::SINK))
      return "sink";
  } else if (role.type == PortRoleType::DATA_ROLE) {
    if (role.role == static_cast<uint32_t>(PortDataRole::HOST)) return "host";
    if (role.role == static_cast<uint32_t>(PortDataRole::DEVICE))
      return "device";
  } else if (role.type == PortRoleType::MODE) {
    if (role.role == static_cast<uint32_t>(PortMode_1_1::UFP)) return "sink";
    if (role.role == static_cast<uint32_t>(PortMode_1_1::DFP)) return "source";
  }
  return "none";
}

static void extractRole(std::string &roleName) {
  std::size_t first, last;

  first = roleName.find("[");
  last = roleName.find("]");

  if (first != std::string::npos && last != std::string::npos) {
    roleName = roleName.substr(first + 1, last - first - 1);
  }
}

static void switchToDrp(const std::string &portName) {
  std::string filename = appendRoleNodeHelper(portName, PortRoleType::MODE);

  if (!WriteStringToFile("dual", filename))
    ALOGE("Fatal: Error while switching back to drp");
}

bool Usb::switchMode(const hidl_string &portName, const PortRole &newRole) {
  std::string filename =
       appendRoleNodeHelper(std::string(portName.c_str()), newRole.type);
  bool roleSwitch = false;

  if (filename == "") {
    ALOGE("Fatal: invalid node type");
    return false;
  }

  {
    // Hold the lock here to prevent loosing connected signals
    // as once the file is written the partner added signal
    // can arrive anytime.
    pthread_mutex_lock(&mPartnerLock);
    mPartnerUp = false;
    if (WriteStringToFile(convertRoletoString(newRole), filename)) {
      struct timespec   to;
      struct timespec   now;

wait_again:
      clock_gettime(CLOCK_MONOTONIC, &now);
      to.tv_sec = now.tv_sec + PORT_TYPE_TIMEOUT;
      to.tv_nsec = now.tv_nsec;

      int err = pthread_cond_timedwait(&mPartnerCV, &mPartnerLock, &to);
      // There are no uevent signals which implies role swap timed out.
      if (err == ETIMEDOUT) {
        ALOGI("uevents wait timedout");
      // Sanity check.
      } else if (!mPartnerUp) {
        goto wait_again;
      // Role switch succeeded since usb->mPartnerUp is true.
      } else {
        roleSwitch = true;
      }
    } else {
      ALOGI("Role switch failed while writing to file");
    }
    pthread_mutex_unlock(&mPartnerLock);
  }

  if (!roleSwitch)
    switchToDrp(std::string(portName.c_str()));

  return roleSwitch;
}

Usb::Usb()
        : mLock(PTHREAD_MUTEX_INITIALIZER),
          mRoleSwitchLock(PTHREAD_MUTEX_INITIALIZER),
          mPartnerLock(PTHREAD_MUTEX_INITIALIZER),
          mPartnerUp(false),
          mContaminantPresence(false) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr)) {
        ALOGE("pthread_condattr_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
        ALOGE("pthread_condattr_setclock failed: %s", strerror(errno));
        abort();
    }
    if (pthread_cond_init(&mPartnerCV, &attr))  {
        ALOGE("pthread_cond_init failed: %s", strerror(errno));
        abort();
    }
    if (pthread_condattr_destroy(&attr)) {
        ALOGE("pthread_condattr_destroy failed: %s", strerror(errno));
        abort();
    }

}


Return<void> Usb::switchRole(const hidl_string &portName,
                             const V1_0::PortRole &newRole) {
  std::string filename =
      appendRoleNodeHelper(std::string(portName.c_str()), newRole.type);
  std::string written;
  bool roleSwitch = false;

  if (filename == "") {
    ALOGE("Fatal: invalid node type");
    return Void();
  }

  pthread_mutex_lock(&mRoleSwitchLock);

  ALOGI("filename write: %s role:%s", filename.c_str(), convertRoletoString(newRole));

  if (newRole.type == PortRoleType::MODE) {
      roleSwitch = switchMode(portName, newRole);
  } else {
    if (WriteStringToFile(convertRoletoString(newRole), filename)) {
      if (ReadFileToString(filename, &written)) {
        extractRole(written);
        ALOGI("written: %s", written.c_str());
        if (written == convertRoletoString(newRole)) {
          roleSwitch = true;
        } else {
          ALOGE("Role switch failed");
        }
      } else {
        ALOGE("Unable to read back the new role");
      }
    } else {
      ALOGE("Role switch failed while writing to file");
    }
  }

  pthread_mutex_lock(&mLock);
  if (mCallback_1_0 != NULL) {
    Return<void> ret =
        mCallback_1_0->notifyRoleSwitchStatus(portName, newRole,
        roleSwitch ? Status::SUCCESS : Status::ERROR);
    if (!ret.isOk())
      ALOGE("RoleSwitchStatus error %s", ret.description().c_str());
  } else {
    ALOGE("Not notifying the userspace. Callback is not set");
  }
  pthread_mutex_unlock(&mLock);
  pthread_mutex_unlock(&mRoleSwitchLock);

  return Void();
}

static Status getAccessoryConnected(const std::string &portName, std::string &accessory) {
  std::string filename = "/sys/class/typec/" + portName + "-partner/accessory_mode";

  if (!ReadFileToString(filename, &accessory)) {
    ALOGE("getAccessoryConnected: Failed to open filesystem node: %s",
          filename.c_str());
    return Status::ERROR;
  }

  accessory = Trim(accessory);
  return Status::SUCCESS;
}

static Status getCurrentRoleHelper(const std::string &portName, bool connected,
                                   PortRoleType type, uint32_t &currentRole) {
  std::string filename;
  std::string roleName;
  std::string accessory;

  // Mode

  if (type == PortRoleType::POWER_ROLE) {
    currentRole = static_cast<uint32_t>(PortPowerRole::NONE);
  } else if (type == PortRoleType::DATA_ROLE) {
    currentRole = static_cast<uint32_t>(PortDataRole::NONE);
  } else if (type == PortRoleType::MODE) {
    currentRole = static_cast<uint32_t>(PortMode_1_1::NONE);
  } else {
    return Status::ERROR;
  }

  if (!connected) return Status::SUCCESS;

  if (type == PortRoleType::MODE) {
    if (getAccessoryConnected(portName, accessory) != Status::SUCCESS) {
      return Status::ERROR;
    }
    if (accessory == "analog_audio") {
      currentRole = static_cast<uint32_t>(PortMode_1_1::AUDIO_ACCESSORY);
      return Status::SUCCESS;
    } else if (accessory == "debug") {
      currentRole = static_cast<uint32_t>(PortMode_1_1::DEBUG_ACCESSORY);
      return Status::SUCCESS;
    }
  }

  filename = appendRoleNodeHelper(portName, type);
  if (!ReadFileToString(filename, &roleName)) {
    ALOGE("getCurrentRole: Failed to open filesystem node: %s",
          filename.c_str());
    return Status::ERROR;
  }

  extractRole(roleName);

  if (roleName == "source") {
    currentRole = static_cast<uint32_t>(PortPowerRole::SOURCE);
  } else if (roleName == "sink") {
    currentRole = static_cast<uint32_t>(PortPowerRole::SINK);
  } else if (roleName == "host") {
    if (type == PortRoleType::DATA_ROLE)
      currentRole = static_cast<uint32_t>(PortDataRole::HOST);
    else
      currentRole = static_cast<uint32_t>(PortMode_1_1::DFP);
  } else if (roleName == "device") {
    if (type == PortRoleType::DATA_ROLE)
      currentRole = static_cast<uint32_t>(PortDataRole::DEVICE);
    else
      currentRole = static_cast<uint32_t>(PortMode_1_1::UFP);
  } else if (roleName != "none") {
    /* case for none has already been addressed.
     * so we check if the role isnt none.
     */
    return Status::UNRECOGNIZED_ROLE;
  }

  return Status::SUCCESS;
}

static std::unordered_map<std::string, bool> getTypeCPortNamesHelper() {
  std::unordered_map<std::string, bool> names;
  DIR *dp;
  dp = opendir("/sys/class/typec");
  if (dp != NULL) {
    struct dirent *ep;

    while ((ep = readdir(dp))) {
      if (ep->d_type == DT_LNK) {
        std::string entry = ep->d_name;
        auto n = entry.find("-partner");
        if (n == std::string::npos) {
          std::unordered_map<std::string, bool>::const_iterator portName =
              names.find(entry);
          if (portName == names.end()) {
            names.insert({entry, false});
          }
        } else {
          names[entry.substr(0, n)] = true;
        }
      }
    }
    closedir(dp);
  } else {
    ALOGE("Failed to open /sys/class/typec");
  }

  return names;
}

static bool canSwitchRoleHelper(const std::string &portName) {
  std::string filename = "/sys/class/typec/" + portName + "-partner/supports_usb_power_delivery";
  std::string supportsPD;

  if (ReadFileToString(filename, &supportsPD)) {
    if (supportsPD[0] == 'y') {
      return true;
    }
  }

  return false;
}

/*
 * The caller of this method would reconstruct the V1_0::PortStatus
 * object if required.
 */
static Status getPortStatusHelper(hidl_vec<PortStatus> &currentPortStatus_1_2,
    bool V1_0, const std::string &contaminantStatusPath) {
  auto names = getTypeCPortNamesHelper();

  if (!names.empty()) {
    currentPortStatus_1_2.resize(names.size());
    int i = 0;
    for (auto & [portName, connected] : names) {
      ALOGI("%s", portName.c_str());
      auto & status_1_2 = currentPortStatus_1_2[i++];
      status_1_2.status_1_1.status.portName = portName;

      uint32_t currentRole;
      if (getCurrentRoleHelper(portName, connected,
                               PortRoleType::POWER_ROLE,
                               currentRole) == Status::SUCCESS) {
        status_1_2.status_1_1.status.currentPowerRole =
            static_cast<PortPowerRole>(currentRole);
      } else {
        ALOGE("Error while retreiving portNames");
        goto done;
      }

      if (getCurrentRoleHelper(portName, connected, PortRoleType::DATA_ROLE,
                               currentRole) == Status::SUCCESS) {
        status_1_2.status_1_1.status.currentDataRole =
            static_cast<PortDataRole>(currentRole);
      } else {
        ALOGE("Error while retreiving current port role");
        goto done;
      }

      if (getCurrentRoleHelper(portName, connected, PortRoleType::MODE,
                               currentRole) == Status::SUCCESS) {
        status_1_2.status_1_1.currentMode =
            static_cast<PortMode_1_1>(currentRole);
        status_1_2.status_1_1.status.currentMode =
            static_cast<V1_0::PortMode>(currentRole);
      } else {
        ALOGE("Error while retreiving current data role");
        goto done;
      }

      status_1_2.status_1_1.status.canChangeMode = true;
      status_1_2.status_1_1.status.canChangeDataRole =
          connected ? canSwitchRoleHelper(portName) : false;
      status_1_2.status_1_1.status.canChangePowerRole =
          connected ? canSwitchRoleHelper(portName) : false;

      ALOGI("connected:%d canChangeMode:%d canChagedata:%d canChangePower:%d",
            connected, status_1_2.status_1_1.status.canChangeMode,
            status_1_2.status_1_1.status.canChangeDataRole,
            status_1_2.status_1_1.status.canChangePowerRole);

      if (V1_0) {
        status_1_2.status_1_1.status.supportedModes = V1_0::PortMode::DFP;
      } else {
        status_1_2.status_1_1.supportedModes =
	    PortMode_1_1::DRP | PortMode_1_1::AUDIO_ACCESSORY;
        status_1_2.status_1_1.status.supportedModes = V1_0::PortMode::NONE;
        status_1_2.status_1_1.status.currentMode = V1_0::PortMode::NONE;

        status_1_2.supportedContaminantProtectionModes =
            ContaminantProtectionMode::FORCE_SINK | ContaminantProtectionMode::FORCE_DISABLE;
        status_1_2.supportsEnableContaminantPresenceProtection = false;
        status_1_2.supportsEnableContaminantPresenceDetection = false;
        status_1_2.contaminantProtectionStatus = ContaminantProtectionStatus::FORCE_SINK;

        if (portName != "port0") // moisture detection only on first port
          continue;

        std::string contaminantPresence;

        if (!contaminantStatusPath.empty() &&
                ReadFileToString(contaminantStatusPath, &contaminantPresence)) {
          if (contaminantPresence[0] == '1') {
            status_1_2.contaminantDetectionStatus = ContaminantDetectionStatus::DETECTED;
            ALOGI("moisture: Contaminant presence detected");
          }
          else {
            status_1_2.contaminantDetectionStatus = ContaminantDetectionStatus::NOT_DETECTED;
          }
        } else {
          status_1_2.supportedContaminantProtectionModes =
              ContaminantProtectionMode::NONE | ContaminantProtectionMode::NONE;
          status_1_2.contaminantProtectionStatus = ContaminantProtectionStatus::NONE;
        }
      }
    }
    return Status::SUCCESS;
  }
done:
  return Status::ERROR;
}

Return<void> Usb::queryPortStatus() {
  hidl_vec<PortStatus> currentPortStatus_1_2;
  hidl_vec<V1_1::PortStatus_1_1> currentPortStatus_1_1;
  hidl_vec<V1_0::PortStatus> currentPortStatus;
  Status status;
  sp<IUsbCallback> callback_V1_2 = IUsbCallback::castFrom(mCallback_1_0);
  sp<V1_1::IUsbCallback> callback_V1_1 = V1_1::IUsbCallback::castFrom(mCallback_1_0);

  pthread_mutex_lock(&mLock);
  if (mCallback_1_0 != NULL) {
    if (callback_V1_1 != NULL) { // 1.1 or 1.2
      if (callback_V1_2 == NULL) { // 1.1 only
        status = getPortStatusHelper(currentPortStatus_1_2, false,
                mContaminantStatusPath);
        currentPortStatus_1_1.resize(currentPortStatus_1_2.size());
        for (unsigned long i = 0; i < currentPortStatus_1_2.size(); i++)
          currentPortStatus_1_1[i].status = currentPortStatus_1_2[i].status_1_1.status;
      }
      else  //1.2 only
        status = getPortStatusHelper(currentPortStatus_1_2, false,
                mContaminantStatusPath);
    } else { // 1.0 only
      status = getPortStatusHelper(currentPortStatus_1_2, true,
              mContaminantStatusPath);
      currentPortStatus.resize(currentPortStatus_1_2.size());
      for (unsigned long i = 0; i < currentPortStatus_1_2.size(); i++)
        currentPortStatus[i] = currentPortStatus_1_2[i].status_1_1.status;
    }

    Return<void> ret;

    if (callback_V1_2 != NULL)
      ret = callback_V1_2->notifyPortStatusChange_1_2(currentPortStatus_1_2, status);
    else if (callback_V1_1 != NULL)
      ret = callback_V1_1->notifyPortStatusChange_1_1(currentPortStatus_1_1, status);
    else
      ret = mCallback_1_0->notifyPortStatusChange(currentPortStatus, status);

    if (!ret.isOk())
      ALOGE("queryPortStatus_1_1 error %s", ret.description().c_str());
  } else {
    ALOGI("Notifying userspace skipped. Callback is NULL");
  }
  pthread_mutex_unlock(&mLock);
  return Void();
}

struct data {
  int uevent_fd;
  android::hardware::usb::V1_2::implementation::Usb *usb;
};

Return<void> callbackNotifyPortStatusChangeHelper(struct Usb *usb) {
  hidl_vec<PortStatus> currentPortStatus_1_2;
  Status status;
  Return<void> ret;
  sp<IUsbCallback> callback_V1_2 = IUsbCallback::castFrom(usb->mCallback_1_0);

  pthread_mutex_lock(&usb->mLock);
  status = getPortStatusHelper(currentPortStatus_1_2, false,
          usb->mContaminantStatusPath);
  ret = callback_V1_2->notifyPortStatusChange_1_2(currentPortStatus_1_2, status);

  if (!ret.isOk())
    ALOGE("notifyPortStatusChange_1_2 error %s", ret.description().c_str());

  pthread_mutex_unlock(&usb->mLock);
  return Void();
}

Return<void> Usb::enableContaminantPresenceDetection(const hidl_string &portName,
                                                     bool enable) {
  Return<void> ret;

  ret = callbackNotifyPortStatusChangeHelper(this);
  ALOGI("Contaminant Presence Detection should always be in enable mode");

  return Void();
}

Return<void> Usb::enableContaminantPresenceProtection(const hidl_string &portName,
                                                      bool enable) {
  Return<void> ret;

  ret = callbackNotifyPortStatusChangeHelper(this);
  ALOGI("Contaminant Presence Protection should always be in enable mode");

  return Void();
}

static void handle_typec_uevent(Usb *usb, const char *msg)
{
  ALOGI("uevent received %s", msg);

  // if (std::regex_match(cp, std::regex("(add)(.*)(-partner)")))
  if (!strncmp(msg, "add@", 4) && !strncmp(msg + strlen(msg) - 8, "-partner", 8)) {
     ALOGI("partner added");
     pthread_mutex_lock(&usb->mPartnerLock);
     usb->mPartnerUp = true;
     pthread_cond_signal(&usb->mPartnerCV);
     pthread_mutex_unlock(&usb->mPartnerLock);
  }

  std::string power_operation_mode;
  if (ReadFileToString("/sys/class/typec/port0/power_operation_mode", &power_operation_mode)) {
    power_operation_mode = Trim(power_operation_mode);
    if (usb->mPowerOpMode == power_operation_mode) {
      ALOGI("uevent recieved for same device %s", power_operation_mode.c_str());
    } else if(power_operation_mode == "usb_power_delivery") {
      ReadFileToString("/config/usb_gadget/g1/configs/b.1/MaxPower", &usb->mMaxPower);
      ReadFileToString("/config/usb_gadget/g1/configs/b.1/bmAttributes", &usb->mAttributes);
      WriteStringToFile("0", "/config/usb_gadget/g1/configs/b.1/MaxPower");
      WriteStringToFile("0xc0", "/config/usb_gadget/g1/configs/b.1/bmAttributes");
    } else {
      if(!usb->mMaxPower.empty()) {
        WriteStringToFile(usb->mMaxPower, "/config/usb_gadget/g1/configs/b.1/MaxPower");
        WriteStringToFile(usb->mAttributes, "/config/usb_gadget/g1/configs/b.1/bmAttributes");
        usb->mMaxPower = "";
      }
    }

    usb->mPowerOpMode = power_operation_mode;
  }

  usb->queryPortStatus();
}

// process POWER_SUPPLY uevent for contaminant presence
static void handle_psy_uevent(Usb *usb, const char *msg)
{
  sp<IUsbCallback> callback_V1_2 = IUsbCallback::castFrom(usb->mCallback_1_0);
  hidl_vec<PortStatus> currentPortStatus_1_2;
  Status status;
  Return<void> ret;
  bool moisture_detected;
  std::string contaminantPresence;

  // don't bother parsing any further if caller doesn't support USB HAL 1.2
  // to report contaminant presence events
  if (callback_V1_2 == NULL)
    return;

  while (*msg) {
    if (!strncmp(msg, "POWER_SUPPLY_NAME=", 18)) {
      msg += 18;
      if (strcmp(msg, "usb")) // make sure we're looking at the correct uevent
        return;
      else
        break;
    }

    // advance to after the next \0
    while (*msg++) ;
  }

  // read moisture detection status from sysfs
  if (usb->mContaminantStatusPath.empty() ||
        !ReadFileToString(usb->mContaminantStatusPath, &contaminantPresence))
    return;

  moisture_detected = (contaminantPresence[0] == '1');

  if (usb->mContaminantPresence != moisture_detected) {
    usb->mContaminantPresence = moisture_detected;

    status = getPortStatusHelper(currentPortStatus_1_2, false,
            usb->mContaminantStatusPath);
    ret = callback_V1_2->notifyPortStatusChange_1_2(currentPortStatus_1_2, status);
    if (!ret.isOk()) ALOGE("error %s", ret.description().c_str());
  }

  //Role switch is not in progress and port is in disconnected state
  if (!pthread_mutex_trylock(&usb->mRoleSwitchLock)) {
    for (unsigned long i = 0; i < currentPortStatus_1_2.size(); i++) {
      DIR *dp = opendir(std::string("/sys/class/typec/"
          + std::string(currentPortStatus_1_2[i].status_1_1.status.portName.c_str())
          + "-partner").c_str());
      if (dp == NULL) {
        //PortRole role = {.role = static_cast<uint32_t>(PortMode::UFP)};
        switchToDrp(currentPortStatus_1_2[i].status_1_1.status.portName);
      } else {
        closedir(dp);
      }
    }
    pthread_mutex_unlock(&usb->mRoleSwitchLock);
  }
}

static void uevent_event(uint32_t /*epevents*/, struct data *payload) {
  char msg[UEVENT_MSG_LEN + 2];
  int n;
  std::string dwc3_sysfs;

  std::string gadgetName = GetProperty(USB_CONTROLLER_PROP, "");
  static std::regex add_regex("add@(/devices/platform/soc/.*dwc3/xhci-hcd\\.\\d\\.auto/"
                              "usb\\d/\\d-\\d(?:/[\\d\\.-]+)*)");
  static std::regex bind_regex("bind@(/devices/platform/soc/.*dwc3/xhci-hcd\\.\\d\\.auto/"
                               "usb\\d/\\d-\\d(?:/[\\d\\.-]+)*)/([^/]*:[^/]*)");
  static std::regex udc_regex("(add|remove)@/devices/platform/soc/.*/" + gadgetName +
                              "/udc/" + gadgetName);
  static std::regex offline_regex("offline@(/devices/platform/.*dwc3/xhci-hcd\\.\\d\\.auto/usb.*)");
  static std::regex dwc3_regex("\\/(\\w+.\\w+usb)/.*dwc3");

  n = uevent_kernel_multicast_recv(payload->uevent_fd, msg, UEVENT_MSG_LEN);
  if (n <= 0) return;
  if (n >= UEVENT_MSG_LEN) /* overflow -- discard */
    return;

  msg[n] = '\0';
  msg[n + 1] = '\0';

  std::cmatch match;

  if (strstr(msg, "typec/port")) {
    handle_typec_uevent(payload->usb, msg);
  } else if (strstr(msg, "power_supply/usb")) {
    handle_psy_uevent(payload->usb, msg + strlen(msg) + 1);
  } else if (std::regex_match(msg, match, add_regex)) {
    if (match.size() == 2) {
      std::csub_match submatch = match[1];
      checkUsbDeviceAutoSuspend("/sys" +  submatch.str());
    }
  } else if (!payload->usb->mIgnoreWakeup && std::regex_match(msg, match, bind_regex)) {
    if (match.size() == 3) {
      std::csub_match devpath = match[1];
      std::csub_match intfpath = match[2];
      checkUsbInterfaceAutoSuspend("/sys" + devpath.str(), intfpath.str());
    }
  } else if (std::regex_match(msg, match, udc_regex)) {
    if (!strncmp(msg, "add", 3)) {
      // Allow ADBD to resume its FFS monitor thread
      SetProperty(VENDOR_USB_ADB_DISABLED_PROP, "0");

      // In case ADB is not enabled, we need to manually re-bind the UDC to
      // ConfigFS since ADBD is not there to trigger it (sys.usb.ffs.ready=1)
      if (GetProperty("init.svc.adbd", "") != "running") {
        ALOGI("Binding UDC %s to ConfigFS", gadgetName.c_str());
        WriteStringToFile(gadgetName, "/config/usb_gadget/g1/UDC");
      }

    } else {
      // When the UDC is removed, the ConfigFS gadget will no longer be
      // bound. If ADBD is running it would keep opening/writing to its
      // FFS EP0 file but since FUNCTIONFS_BIND doesn't happen it will
      // just keep repeating this in a 1 second retry loop. Each iteration
      // will re-trigger a ConfigFS UDC bind which will keep failing.
      // Setting this property stops ADBD from proceeding with the retry.
      SetProperty(VENDOR_USB_ADB_DISABLED_PROP, "1");
    }
  }  else if (std::regex_match(msg, match, offline_regex)) {
	 if(std::regex_search (msg, match, dwc3_regex))
	 {
		 dwc3_sysfs = USB_MODE_PATH + match.str(1) + "/mode";
		 ALOGE("ERROR:restarting in host mode");
		 WriteStringToFile("none", dwc3_sysfs);
		 sleep(1);
		 WriteStringToFile("host", dwc3_sysfs);
	 }
 }
}

static void *work(void *param) {
  int epoll_fd, uevent_fd;
  struct epoll_event ev;
  int nevents = 0;
  struct data payload;

  ALOGE("creating thread");

  uevent_fd = uevent_open_socket(64 * 1024, true);

  if (uevent_fd < 0) {
    ALOGE("uevent_init: uevent_open_socket failed\n");
    return NULL;
  }

  payload.uevent_fd = uevent_fd;
  payload.usb = (android::hardware::usb::V1_2::implementation::Usb *)param;

  fcntl(uevent_fd, F_SETFL, O_NONBLOCK);

  ev.events = EPOLLIN;
  ev.data.ptr = (void *)uevent_event;

  epoll_fd = epoll_create(64);
  if (epoll_fd == -1) {
    ALOGE("epoll_create failed; errno=%d", errno);
    goto error;
  }

  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, uevent_fd, &ev) == -1) {
    ALOGE("epoll_ctl failed; errno=%d", errno);
    goto error;
  }

  while (!destroyThread) {
    struct epoll_event events[64];

    nevents = epoll_wait(epoll_fd, events, 64, -1);
    if (nevents == -1) {
      if (errno == EINTR) continue;
      ALOGE("usb epoll_wait failed; errno=%d", errno);
      break;
    }

    for (int n = 0; n < nevents; ++n) {
      if (events[n].data.ptr)
        (*(void (*)(uint32_t, struct data *payload))events[n].data.ptr)(
            events[n].events, &payload);
    }
  }

  ALOGI("exiting worker thread");
error:
  close(uevent_fd);

  if (epoll_fd >= 0) close(epoll_fd);

  return NULL;
}

static void sighandler(int sig) {
  if (sig == SIGUSR1) {
    destroyThread = true;
    ALOGI("destroy set");
    return;
  }
  signal(SIGUSR1, sighandler);
}

Return<void> Usb::setCallback(const sp<V1_0::IUsbCallback> &callback) {

  sp<V1_1::IUsbCallback> callback_V1_1 = V1_1::IUsbCallback::castFrom(callback);
  sp<IUsbCallback> callback_V1_2 = IUsbCallback::castFrom(callback);

  if (callback != NULL)
      if (callback_V1_1 == NULL)
          ALOGI("Registering 1.0 callback");

  pthread_mutex_lock(&mLock);
  /*
   * When both the old callback and new callback values are NULL,
   * there is no need to spin off the worker thread.
   * When both the values are not NULL, we would already have a
   * worker thread running, so updating the callback object would
   * be suffice.
   */
  if ((mCallback_1_0 == NULL && callback == NULL) ||
      (mCallback_1_0 != NULL && callback != NULL)) {
    /*
     * Always store as V1_0 callback object. Type cast to V1_1
     * when the callback is actually invoked.
     */
    mCallback_1_0 = callback;
    pthread_mutex_unlock(&mLock);
    return Void();
  }

  mCallback_1_0 = callback;
  ALOGI("registering callback");

  // Kill the worker thread if the new callback is NULL.
  if (mCallback_1_0 == NULL) {
    pthread_mutex_unlock(&mLock);
    if (!pthread_kill(mPoll, SIGUSR1)) {
      pthread_join(mPoll, NULL);
      ALOGI("pthread destroyed");
    }
    return Void();
  }

  destroyThread = false;
  signal(SIGUSR1, sighandler);

  /*
   * Create a background thread if the old callback value is NULL
   * and being updated with a new value.
   */
  if (pthread_create(&mPoll, NULL, work, this)) {
    ALOGE("pthread creation failed %d", errno);
    mCallback_1_0 = NULL;
  }

  pthread_mutex_unlock(&mLock);

  mIgnoreWakeup = checkUsbWakeupSupport();
  checkUsbInHostMode();

  /*
   * Check for the correct path to detect contaminant presence status
   * from the possible paths and use that to get contaminant
   * presence status when required.
   */
  if (access("/sys/class/power_supply/usb/moisture_detected", R_OK) == 0) {
    mContaminantStatusPath = "/sys/class/power_supply/usb/moisture_detected";
  } else if (access("/sys/class/qcom-battery/moisture_detection_status", R_OK) == 0) {
    mContaminantStatusPath = "/sys/class/qcom-battery/moisture_detection_status";
  } else if (access("/sys/bus/iio/devices/iio:device4/in_index_usb_moisture_detected_input", R_OK) == 0) {
    mContaminantStatusPath = "/sys/bus/iio/devices/iio:device4/in_index_usb_moisture_detected_input";
  } else {
    mContaminantStatusPath.clear();
  }

  ALOGI("Contamination presence path: %s", mContaminantStatusPath.c_str());

  return Void();
}

static void checkUsbInHostMode() {
  std::string gadgetName = "/sys/bus/platform/devices/" + GetProperty(USB_CONTROLLER_PROP, "");
  DIR *gd = opendir(gadgetName.c_str());
  if (gd != NULL) {
    struct dirent *gadgetDir;
    while ((gadgetDir = readdir(gd))) {
      if (strstr(gadgetDir->d_name, "xhci-hcd")) {
        SetProperty(VENDOR_USB_ADB_DISABLED_PROP, "1");
        closedir(gd);
        return;
      }
    }
    closedir(gd);
  }
  SetProperty(VENDOR_USB_ADB_DISABLED_PROP, "0");
}

static bool checkUsbWakeupSupport() {
  std::string platdevices = "/sys/bus/platform/devices/";
  DIR *pd = opendir(platdevices.c_str());
  bool ignoreWakeup = true;

  if (pd != NULL) {
    struct dirent *platDir;
    while ((platDir = readdir(pd))) {
      std::string cname = platDir->d_name;
      /*
       * Scan for USB controller. Here "susb" takes care of both hsusb and ssusb.
       * Set mIgnoreWakeup based on the availability of 1st Controller's
       * power/wakeup node.
       */
      if (strstr(platDir->d_name, "susb")) {
        if (faccessat(dirfd(pd), (cname + "/power/wakeup").c_str(), F_OK, 0) < 0) {
          ignoreWakeup = true;
          ALOGI("PLATFORM DOESN'T SUPPORT WAKEUP");
        } else {
          ignoreWakeup = false;
        }
        break;
      }
    }
    closedir(pd);
  }

  if (ignoreWakeup)
    return true;

  /*
   * If wakeup is supported then scan for enumerated USB devices and
   * enable autosuspend.
   */
  std::string usbdevices = "/sys/bus/usb/devices/";
  DIR *dp = opendir(usbdevices.c_str());
  if (dp != NULL) {
    struct dirent *deviceDir;
    struct dirent *intfDir;
    DIR *ip;

    while ((deviceDir = readdir(dp))) {
      /*
       * Iterate over all the devices connected over USB while skipping
       * the interfaces.
       */
      if (deviceDir->d_type == DT_LNK && !strchr(deviceDir->d_name, ':')) {
        char buf[PATH_MAX];
        if (realpath((usbdevices + deviceDir->d_name).c_str(), buf)) {

          ip = opendir(buf);
          if (ip == NULL)
            continue;

          while ((intfDir = readdir(ip))) {
            // Scan over all the interfaces that are part of the device
            if (intfDir->d_type == DT_DIR && strchr(intfDir->d_name, ':')) {
              /*
               * If the autosuspend is successfully enabled, no need
               * to iterate over other interfaces.
               */
              if (checkUsbInterfaceAutoSuspend(buf, intfDir->d_name))
                break;
            }
          }
          closedir(ip);
        }
      }
    }
    closedir(dp);
  }

  return ignoreWakeup;
}

/*
 * allow specific USB device idProduct and idVendor to auto suspend
 */
static bool canProductAutoSuspend(const std::string &deviceIdVendor,
    const std::string &deviceIdProduct) {
  if (deviceIdVendor == GOOGLE_USB_VENDOR_ID_STR &&
      deviceIdProduct == GOOGLE_USBC_35_ADAPTER_UNPLUGGED_ID_STR) {
    return true;
  }
  return false;
}

static bool canUsbDeviceAutoSuspend(const std::string &devicePath) {
  std::string deviceIdVendor;
  std::string deviceIdProduct;
  ReadFileToString(devicePath + "/idVendor", &deviceIdVendor);
  ReadFileToString(devicePath + "/idProduct", &deviceIdProduct);

  // deviceIdVendor and deviceIdProduct will be empty strings if ReadFileToString fails
  return canProductAutoSuspend(Trim(deviceIdVendor), Trim(deviceIdProduct));
}

/*
 * function to consume USB device plugin events (on receiving a
 * USB device path string), and enable autosupend on the USB device if
 * necessary.
 */
static void checkUsbDeviceAutoSuspend(const std::string& devicePath) {
  /*
   * Currently we only actively enable devices that should be autosuspended, and leave others
   * to the defualt.
   */
  if (canUsbDeviceAutoSuspend(devicePath)) {
    ALOGI("auto suspend usb device %s", devicePath.c_str());
    WriteStringToFile("auto", devicePath + "/power/control");
    WriteStringToFile("enabled", devicePath + "/power/wakeup");
  }
}

static bool checkUsbInterfaceAutoSuspend(const std::string& devicePath,
        const std::string &intf) {
  std::string bInterfaceClass;
  int interfaceClass, retry = 3;
  bool ret = false;

  do {
    ReadFileToString(devicePath + "/" + intf + "/bInterfaceClass", &bInterfaceClass);
  } while ((--retry > 0) && (bInterfaceClass.length() == 0));

  if (bInterfaceClass.length() == 0) {
	  return false;
  }
  interfaceClass = std::stoi(bInterfaceClass, 0, 16);

  // allow autosuspend for certain class devices
  switch (interfaceClass) {
    case USB_CLASS_AUDIO:
    case USB_CLASS_HUB:
      ALOGI("auto suspend usb interfaces %s", devicePath.c_str());
      ret = WriteStringToFile("auto", devicePath + "/power/control");
      if (!ret)
        break;

      ret = WriteStringToFile("enabled", devicePath + "/power/wakeup");
      break;
     default:
      ALOGI("usb interface does not support autosuspend %s", devicePath.c_str());

  }

  return ret;
}

}  // namespace implementation
}  // namespace V1_2
}  // namespace usb
}  // namespace hardware
}  // namespace android

int main() {
  using android::hardware::configureRpcThreadpool;
  using android::hardware::joinRpcThreadpool;
  using android::hardware::usb::V1_2::IUsb;
  using android::hardware::usb::V1_2::implementation::Usb;

  android::sp<IUsb> service = new Usb();

  configureRpcThreadpool(1, true /*callerWillJoin*/);
  android::status_t status = service->registerAsService();

  if (status != android::OK) {
    ALOGE("Cannot register USB HAL service");
    return 1;
  }

  ALOGI("QTI USB HAL Ready.");
  joinRpcThreadpool();
  // Under normal cases, execution will not reach this line.
  ALOGI("QTI USB HAL failed to join thread pool.");
  return 1;
}
