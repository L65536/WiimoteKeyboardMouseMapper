#include "ConfigManager.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr WORD MOUSE_WHEEL_UP = 0x1000;
constexpr WORD MOUSE_WHEEL_DOWN = 0x1001;
}

ConfigManager::ConfigManager() {
  // Ensure profiles directory exists
  try {
    if (!fs::exists("profiles")) {
      fs::create_directory("profiles");
    }
  } catch (...) {
  }

  // Initialize with some default-like behavior
  m_mappings["A"] = {VK_SPACE};
  m_mappings["B"] = {VK_ESCAPE};
  m_mappings["UP"] = {VK_UP};
  m_mappings["DOWN"] = {VK_DOWN};
  m_mappings["LEFT"] = {VK_LEFT};
  m_mappings["RIGHT"] = {VK_RIGHT};
  m_mappings["PLUS"] = {VK_ADD};
  m_mappings["MINUS"] = {VK_SUBTRACT};
  m_mappings["HOME"] = {VK_LWIN};
  m_mappings["ONE"] = {'1'};
  m_mappings["TWO"] = {'2'};

  // If a profile doesn't exist yet, save the initial state as 'default'
  if (!fs::exists("profiles/default.json")) {
    SaveProfile("default");
  }

  // Load the last active session
  LoadActiveConfig();
}

void ConfigManager::LoadActiveConfig() {
  if (!fs::exists("active_config.json")) {
    // If no active session, start from default profile
    LoadProfile("default");
    return;
  }

  std::ifstream file("active_config.json");
  if (!file.is_open())
    return;

  try {
    json j;
    file >> j;
    if (j.contains("mappings")) {
      m_mappings.clear();
      for (auto &[key, val] : j["mappings"].items()) {
        std::vector<WORD> keys;
        if (val.is_array()) {
          for (auto &k : val)
            keys.push_back(StringToVK(k.get<std::string>()));
        } else if (val.is_string()) {
          keys.push_back(StringToVK(val.get<std::string>()));
        }
        m_mappings[key] = keys;
      }
    }
    if (j.contains("last_profile")) {
      m_loadedProfileName = j["last_profile"].get<std::string>();
    }
    if (j.contains("minimize_to_tray")) {
      m_minimizeToTray = j["minimize_to_tray"].get<bool>();
    }
  } catch (...) {
  }
}

void ConfigManager::SaveActiveConfig() {
  json j;
  for (const auto &[btn, keys] : m_mappings) {
    json keyArray = json::array();
    for (WORD vk : keys)
      keyArray.push_back(VKToString(vk));
    j["mappings"][btn] = keyArray;
  }
  j["last_profile"] = m_loadedProfileName;
  j["minimize_to_tray"] = m_minimizeToTray;

  std::ofstream file("active_config.json");
  if (file.is_open()) {
    file << j.dump(4);
  }
}

void ConfigManager::LoadProfile(const std::string &profileName) {
  std::string filename = "profiles/" + profileName + ".json";
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cout << "Profile not found: " << profileName << std::endl;
    return;
  }

  try {
    json j;
    file >> j;
    if (j.contains("mappings")) {
      m_mappings.clear();
      for (auto &[key, val] : j["mappings"].items()) {
        std::vector<WORD> keys;
        if (val.is_array()) {
          for (auto &k : val)
            keys.push_back(StringToVK(k.get<std::string>()));
        } else if (val.is_string()) {
          keys.push_back(StringToVK(val.get<std::string>()));
        }
        m_mappings[key] = keys;
      }
    }
    m_loadedProfileName = profileName;
    SaveActiveConfig(); // Sync active session with newly loaded profile
    std::cout << "Imported profile: " << profileName << std::endl;
  } catch (...) {
  }
}

void ConfigManager::SaveProfile(const std::string &profileName) {
  try {
    if (!fs::exists("profiles"))
      fs::create_directory("profiles");
  } catch (...) {
  }

  std::string filename = "profiles/" + profileName + ".json";

  json j;
  for (const auto &[btn, keys] : m_mappings) {
    json keyArray = json::array();
    for (WORD vk : keys)
      keyArray.push_back(VKToString(vk));
    j["mappings"][btn] = keyArray;
  }

  std::ofstream file(filename);
  if (file.is_open()) {
    file << j.dump(4);
    m_loadedProfileName = profileName;
    SaveActiveConfig(); // Also update active config tracking
    std::cout << "Exported current settings to profile: " << profileName
              << std::endl;
  }
}

void ConfigManager::SaveCurrentProfile() {
  SaveProfile(m_loadedProfileName);
}

bool ConfigManager::LoadProfileFromPath(const std::string &filePath) {
  std::ifstream file(filePath);
  if (!file.is_open())
    return false;

  try {
    json j;
    file >> j;
    if (!j.contains("mappings"))
      return false;

    m_mappings.clear();
    for (auto &[key, val] : j["mappings"].items()) {
      std::vector<WORD> keys;
      if (val.is_array()) {
        for (auto &k : val)
          keys.push_back(StringToVK(k.get<std::string>()));
      } else if (val.is_string()) {
        keys.push_back(StringToVK(val.get<std::string>()));
      }
      m_mappings[key] = keys;
    }

    // Derive profile name from filename and copy into profiles/ dir
    std::string name = fs::path(filePath).stem().string();
    std::string dest = "profiles/" + name + ".json";
    try {
      if (!fs::exists("profiles"))
        fs::create_directory("profiles");
      fs::copy_file(filePath, dest, fs::copy_options::overwrite_existing);
    } catch (...) {
    }

    m_loadedProfileName = name;
    SaveActiveConfig();
    std::cout << "Loaded profile from file: " << filePath << std::endl;
    return true;
  } catch (...) {
    return false;
  }
}

bool ConfigManager::SaveProfileToPath(const std::string &filePath) {
  json j;
  for (const auto &[btn, keys] : m_mappings) {
    json keyArray = json::array();
    for (WORD vk : keys)
      keyArray.push_back(VKToString(vk));
    j["mappings"][btn] = keyArray;
  }

  std::ofstream file(filePath);
  if (!file.is_open())
    return false;

  file << j.dump(4);
  std::cout << "Saved profile to file: " << filePath << std::endl;
  return true;
}

std::vector<std::string> ConfigManager::ListProfiles() const {
  std::vector<std::string> profiles;
  try {
    if (fs::exists("profiles")) {
      for (const auto &entry : fs::directory_iterator("profiles")) {
        if (entry.path().extension() == ".json") {
          profiles.push_back(entry.path().stem().string());
        }
      }
    }
  } catch (...) {
  }
  if (profiles.empty())
    profiles.push_back("default");
  return profiles;
}

void ConfigManager::DeleteProfile(const std::string &profileName) {
  if (profileName == "default")
    return;
  std::string filename = "profiles/" + profileName + ".json";
  try {
    if (fs::exists(filename))
      fs::remove(filename);
  } catch (...) {
  }
}

const std::vector<WORD> &
ConfigManager::GetMappedKeys(const std::string &buttonName) const {
  static const std::vector<WORD> empty;
  auto it = m_mappings.find(buttonName);
  if (it != m_mappings.end())
    return it->second;
  return empty;
}

void ConfigManager::SetMappedKeys(const std::string &buttonName,
                                  const std::vector<WORD> &keys) {
  m_mappings[buttonName] = keys;
  SaveActiveConfig(); // Auto-save all changes to active_config.json
}

void ConfigManager::SetMinimizeToTray(bool enable) {
  m_minimizeToTray = enable;
  SaveActiveConfig();
}

WORD ConfigManager::StringToVK(const std::string &keyStr) const {
  if (keyStr == "NONE")
    return 0;
  if (keyStr == "Space")
    return VK_SPACE;
  if (keyStr == "Enter")
    return VK_RETURN;
  if (keyStr == "Esc")
    return VK_ESCAPE;
  if (keyStr == "Up")
    return VK_UP;
  if (keyStr == "Down")
    return VK_DOWN;
  if (keyStr == "Left")
    return VK_LEFT;
  if (keyStr == "Right")
    return VK_RIGHT;
  if (keyStr == "L-Shift")
    return VK_LSHIFT;
  if (keyStr == "R-Shift")
    return VK_RSHIFT;
  if (keyStr == "L-Ctrl")
    return VK_LCONTROL;
  if (keyStr == "R-Ctrl")
    return VK_RCONTROL;
  if (keyStr == "L-Alt")
    return VK_LMENU;
  if (keyStr == "R-Alt")
    return VK_RMENU;
  if (keyStr == "Win")
    return VK_LWIN;
  if (keyStr == "Right Win")
    return VK_RWIN;
  if (keyStr == "Mouse Left")
    return VK_LBUTTON;
  if (keyStr == "Mouse Right")
    return VK_RBUTTON;
  if (keyStr == "Mouse Middle")
    return VK_MBUTTON;
  if (keyStr == "Mouse Wheel Up")
    return MOUSE_WHEEL_UP;
  if (keyStr == "Mouse Wheel Down")
    return MOUSE_WHEEL_DOWN;
  if (keyStr == "Volume Mute")
    return VK_VOLUME_MUTE;
  if (keyStr == "Volume Up")
    return VK_VOLUME_UP;
  if (keyStr == "Volume Down")
    return VK_VOLUME_DOWN;

  for (int i = 1; i < 256; ++i) {
    if (VKToString((WORD)i) == keyStr)
      return (WORD)i;
  }
  return 0;
}

std::string ConfigManager::VKToString(WORD vk) const {
  if (vk == 0)
    return "NONE";
  switch (vk) {
  case VK_SPACE:
    return "Space";
  case VK_RETURN:
    return "Enter";
  case VK_ESCAPE:
    return "Esc";
  case VK_LSHIFT:
    return "L-Shift";
  case VK_RSHIFT:
    return "R-Shift";
  case VK_LCONTROL:
    return "L-Ctrl";
  case VK_RCONTROL:
    return "R-Ctrl";
  case VK_LMENU:
    return "L-Alt";
  case VK_RMENU:
    return "R-Alt";
  case VK_LWIN:
    return "Win";
  case VK_RWIN:
    return "Right Win";
  case VK_LBUTTON:
    return "Mouse Left";
  case VK_RBUTTON:
    return "Mouse Right";
  case VK_MBUTTON:
    return "Mouse Middle";
  case MOUSE_WHEEL_UP:
    return "Mouse Wheel Up";
  case MOUSE_WHEEL_DOWN:
    return "Mouse Wheel Down";
  case VK_VOLUME_MUTE:
    return "Volume Mute";
  case VK_VOLUME_UP:
    return "Volume Up";
  case VK_VOLUME_DOWN:
    return "Volume Down";
  }

  char name[64];
  UINT scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
  LONG lParam = (scanCode << 16);
  if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
      vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_UP ||
      vk == VK_RIGHT || vk == VK_DOWN || vk == VK_RCONTROL || vk == VK_RMENU) {
    lParam |= (1 << 24);
  }

  if (GetKeyNameTextA(lParam, name, sizeof(name)) > 0)
    return std::string(name);
  char hex[16];
  sprintf_s(hex, "0x%02X", vk);
  return std::string(hex);
}
