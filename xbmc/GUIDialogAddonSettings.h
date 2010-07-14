#pragma once
/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "GUIDialogBoxBase.h"
#include "addons/Addon.h"

class CGUIDialogAddonSettings : public CGUIDialogBoxBase
{
public:
  CGUIDialogAddonSettings(void);
  virtual ~CGUIDialogAddonSettings(void);
  virtual bool OnMessage(CGUIMessage& message);
  /*! \brief Show the addon settings dialog, allowing the user to configure an addon
   \param addon the addon to configure
   \param saveToDisk whether the changes should be saved to disk or just made local to the addon.  Defaults to true
   \return true if settings were changed and the dialog confirmed, false otherwise.
   */
  static bool ShowAndGetInput(const ADDON::AddonPtr &addon, bool saveToDisk = true);
  virtual void Render();

protected:
  virtual void OnInitWindow();

private:
  /*! \brief return a (localized) addon string.
   \param value either a character string (which is used directly) or a number to lookup in the addons strings.xml
   \param subsetting whether the character string should be prefixed by "- ", defaults to false
   \return the localized addon string
   */
  CStdString GetString(const char *value, bool subSetting = false) const;

  /*! \brief return a the values for a fileenum setting
   \param path the path to use for files
   \param mask the mask to use
   \return the filenames in the path that match the mask
   */
  std::vector<CStdString> GetFileEnumValues(const CStdString &path, const CStdString &mask) const;

  void CreateSections();
  void FreeSections();
  void CreateControls();
  void FreeControls();
  void UpdateFromControls();
  void EnableControls();
  void SetDefaults();
  bool GetCondition(const CStdString &condition, const int controlId);

  void SaveSettings(void);
  bool ShowVirtualKeyboard(int iControl);
  bool TranslateSingleString(const CStdString &strCondition, std::vector<CStdString> &enableVec);

  const TiXmlElement *GetFirstSetting() const;

  ADDON::AddonPtr m_addon;
  CStdString m_strHeading;
  std::map<CStdString,CStdString> m_buttonValues;
  bool m_changed;
  bool m_saveToDisk; // whether the addon settings should be saved to disk or just stored locally in the addon

  unsigned int m_currentSection;
  unsigned int m_totalSections;

  std::map<CStdString,CStdString> m_settings; // local storage of values
};

