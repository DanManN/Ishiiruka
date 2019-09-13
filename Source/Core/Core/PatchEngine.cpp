// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// PatchEngine
// Supports simple memory patches, and has a partial Action Replay implementation
// in ActionReplay.cpp/h.

#include "Core/PatchEngine.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Common/Assert.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/ActionReplay.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PowerPC/PowerPC.h"

namespace PatchEngine
{
const char* PatchTypeStrings[] = {
    "byte", "word", "dword",
};

static std::vector<Patch> onFrame;
static std::map<u32, int> speedHacks;

static bool IsEnabledMusicCode(const Patch& patch)
{
  if (SConfig::GetInstance().bBrawlMusicOff && patch.name == "[P+] Music Off")
    return true;
  //PowerPC::HostWrite_U8((u8)00000000, 0x90E60F34);
}
/*
static bool IsDisabledMusicCode(const Patch& patch)
{
  return false;
}
*/
void LoadPatchSection(const std::string& section, std::vector<Patch>& patches, IniFile& globalIni,
                      IniFile& localIni)
{
  // Load the name of all enabled patches
  std::string enabledSectionName = section + "_Enabled";
  std::vector<std::string> enabledLines;
  std::set<std::string> enabledNames;
  localIni.GetLines(enabledSectionName, &enabledLines);
  for (const std::string& line : enabledLines)
  {
    if (line.size() != 0 && line[0] == '$')
    {
      std::string name = line.substr(1, line.size() - 1);
      enabledNames.insert(name);
    }
  }

  const IniFile* inis[2] = {&globalIni, &localIni};

  for (const IniFile* ini : inis)
  {
    std::vector<std::string> lines;
    Patch currentPatch;
    ini->GetLines(section, &lines);

    for (std::string& line : lines)
    {
      if (line.size() == 0)
        continue;

      if (line[0] == '$')
      {
        // Take care of the previous code
        if (currentPatch.name.size())
        {
          patches.push_back(currentPatch);
        }
        currentPatch.entries.clear();

        // Set active and name
        currentPatch.name = line.substr(1, line.size() - 1);
        currentPatch.active = enabledNames.find(currentPatch.name) != enabledNames.end();
        currentPatch.user_defined = (ini == &localIni);
      }
      else
      {
        std::string::size_type loc = line.find('=');

        if (loc != std::string::npos)
        {
          line[loc] = ':';
        }

        const std::vector<std::string> items = SplitString(line, ':');

        if (items.size() >= 3)
        {
          PatchEntry pE;
          bool success = true;
          success &= TryParse(items[0], &pE.address);
          success &= TryParse(items[2], &pE.value);

          pE.type = PatchType(std::find(PatchTypeStrings, PatchTypeStrings + 3, items[1]) -
                              PatchTypeStrings);
          success &= (pE.type != (PatchType)3);
          if (success)
          {
            currentPatch.entries.push_back(pE);
          }
        }
      }
    }

    if (currentPatch.name.size() && currentPatch.entries.size())
    {
      patches.push_back(currentPatch);
    }
  }
}

static void LoadSpeedhacks(const std::string& section, IniFile& ini)
{
  std::vector<std::string> keys;
  ini.GetKeys(section, &keys);
  for (const std::string& key : keys)
  {
    std::string value;
    ini.GetOrCreateSection(section)->Get(key, &value, "BOGUS");
    if (value != "BOGUS")
    {
      u32 address;
      u32 cycles;
      bool success = true;
      success &= TryParse(key, &address);
      success &= TryParse(value, &cycles);
      if (success)
      {
        speedHacks[address] = (int)cycles;
      }
    }
  }
}

int GetSpeedhackCycles(const u32 addr)
{
  std::map<u32, int>::const_iterator iter = speedHacks.find(addr);
  if (iter == speedHacks.end())
    return 0;
  else
    return iter->second;
}

void LoadPatches()
{
  IniFile merged = SConfig::GetInstance().LoadGameIni();
  IniFile globalIni = SConfig::GetInstance().LoadDefaultGameIni();
  IniFile localIni = SConfig::GetInstance().LoadLocalGameIni();

  LoadPatchSection("OnFrame", onFrame, globalIni, localIni);
  ActionReplay::LoadAndApplyCodes(globalIni, localIni);

  Gecko::SetActiveCodes(Gecko::LoadCodes(globalIni, localIni));

  LoadSpeedhacks("Speedhacks", merged);
}

static void ApplyPatches(const std::vector<Patch>& patches)
{
  for (const Patch& patch : patches)
  {
    if (patch.active || IsEnabledMusicCode(patch))
    {
      for (const PatchEntry& entry : patch.entries)
      {
        u32 addr = entry.address;
        u32 value = entry.value;
        switch (entry.type)
        {
        case PATCH_8BIT:
          PowerPC::HostWrite_U8((u8)value, addr);
          break;
        case PATCH_16BIT:
          PowerPC::HostWrite_U16((u16)value, addr);
          break;
        case PATCH_32BIT:
          PowerPC::HostWrite_U32(value, addr);
          break;
        default:
          // unknown patchtype
          break;
        }
      }
    }
  }
}

// Requires MSR.DR, MSR.IR
// There's no perfect way to do this, it's just a heuristic.
// We require at least 2 stack frames, if the stack is shallower than that then it won't work.
static bool IsStackSane()
{
  DEBUG_ASSERT(UReg_MSR(MSR).DR && UReg_MSR(MSR).IR);

  // Check the stack pointer
  u32 SP = GPR(1);
  if (!PowerPC::HostIsRAMAddress(SP))
    return false;

  // Read the frame pointer from the stack (find 2nd frame from top), assert that it makes sense
  u32 next_SP = PowerPC::HostRead_U32(SP);
  if (next_SP <= SP || !PowerPC::HostIsRAMAddress(next_SP) ||
      !PowerPC::HostIsRAMAddress(next_SP + 4))
    return false;

  // Check the link register makes sense (that it points to a valid IBAT address)
  const u32 address = PowerPC::HostRead_U32(next_SP + 4);
  return PowerPC::HostIsInstructionRAMAddress(address) && 0 != PowerPC::HostRead_U32(address);
}

bool ApplyFramePatches()
{
  // Because we're using the VI Interrupt to time this instead of patching the game with a
  // callback hook we can end up catching the game in an exception vector.
  // We deal with this by returning false so that SystemTimers will reschedule us in a few cycles
  // where we can try again after the CPU hopefully returns back to the normal instruction flow.
  UReg_MSR msr = MSR;
  if (!msr.DR || !msr.IR || !IsStackSane())
  {
    DEBUG_LOG(
        ACTIONREPLAY,
        "Need to retry later. CPU configuration is currently incorrect. PC = 0x%08X, MSR = 0x%08X",
        PC, MSR);
    return false;
  }

  ApplyPatches(onFrame);

  // Run the Gecko code handler
  Gecko::RunCodeHandler();
  ActionReplay::RunAllActive();

  return true;
}

void Shutdown()
{
  onFrame.clear();
  speedHacks.clear();
  ActionReplay::ApplyCodes({});
  Gecko::Shutdown();
}

void Reload()
{
  Shutdown();
  LoadPatches();
}

}  // namespace
