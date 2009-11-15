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
#include "system.h"
#ifdef __APPLE__
#include <sys/param.h>
#include <mach-o/dyld.h>
#endif

#ifdef _LINUX
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#include "Application.h"
#include "AutoPtrHandle.h"
#include "Util.h"
#include "utils/IoSupport.h"
#include "FileSystem/StackDirectory.h"
#include "FileSystem/VirtualPathDirectory.h"
#include "FileSystem/MultiPathDirectory.h"
#include "FileSystem/DirectoryCache.h"
#include "FileSystem/SpecialProtocol.h"
#include "ThumbnailCache.h"
#include "FileSystem/RarManager.h"
#include "FileSystem/CMythDirectory.h"
#ifdef HAS_UPNP
#include "FileSystem/UPnPDirectory.h"
#endif
#ifdef HAS_CREDITS
#include "Credits.h"
#endif
#ifdef HAS_VIDEO_PLAYBACK
#include "cores/VideoRenderers/RenderManager.h"
#endif
#include "utils/RegExp.h"
#include "utils/RssFeed.h"
#include "GUISettings.h"
#include "TextureManager.h"
#include "utils/fstrcmp.h"
#include "MediaManager.h"
#include "DirectXGraphics.h"
#include "DNSNameCache.h"
#include "GUIWindowManager.h"
#ifdef _WIN32
#include <shlobj.h>
#include "WIN32Util.h"
#endif
#if defined(__APPLE__)
#include "CocoaInterface.h"
#endif
#include "GUIDialogYesNo.h"
#include "GUIUserMessages.h"
#include "FileSystem/File.h"
#include "Crc32.h"
#include "Settings.h"
#include "StringUtils.h"
#include "AdvancedSettings.h"
#ifdef HAS_LIRC
#include "common/LIRC.h"
#endif
#ifdef HAS_IRSERVERSUITE
  #include "common/IRServerSuite/IRServerSuite.h"
#endif
#include "WindowingFactory.h"
#include "LocalizeStrings.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"

using namespace std;
using namespace DIRECTORY;

#define clamp(x) (x) > 255.f ? 255 : ((x) < 0 ? 0 : (BYTE)(x+0.5f)) // Valid ranges: brightness[-1 -> 1 (0 is default)] contrast[0 -> 2 (1 is default)]  gamma[0.5 -> 3.5 (1 is default)] default[ramp is linear]
static const __int64 SECS_BETWEEN_EPOCHS = 11644473600LL;
static const __int64 SECS_TO_100NS = 10000000;

using namespace AUTOPTR;
using namespace XFILE;
using namespace PLAYLIST;

#ifdef HAS_DX
static D3DGAMMARAMP oldramp, flashramp;
#elif defined(HAS_SDL_2D)
static uint16_t oldrampRed[256];
static uint16_t oldrampGreen[256];
static uint16_t oldrampBlue[256];
static uint16_t flashrampRed[256];
static uint16_t flashrampGreen[256];
static uint16_t flashrampBlue[256];
#endif

CUtil::CUtil(void)
{
}

CUtil::~CUtil(void)
{}

/* returns filename extension including period of filename */
const CStdString CUtil::GetExtension(const CStdString& strFileName)
{
  if(strFileName.Find("://") >= 0)
  {
    CURL url(strFileName);
    return CUtil::GetExtension(url.GetFileName());
  }

  int period = strFileName.find_last_of('.');
  if(period >= 0)
  {
    if( strFileName.find_first_of('/', period+1) != string::npos ) return "";
    if( strFileName.find_first_of('\\', period+1) != string::npos ) return "";
    return strFileName.substr(period);
  }
  else
    return "";
}

void CUtil::GetExtension(const CStdString& strFile, CStdString& strExtension)
{
  strExtension = GetExtension(strFile);
}

/* returns a filename given an url */
/* handles both / and \, and options in urls*/
const CStdString CUtil::GetFileName(const CStdString& strFileNameAndPath)
{
  if(strFileNameAndPath.Find("://") >= 0)
  {
    CURL url(strFileNameAndPath);
    return CUtil::GetFileName(url.GetFileName());
  }

  /* find any slashes */
  const int slash1 = strFileNameAndPath.find_last_of('/');
  const int slash2 = strFileNameAndPath.find_last_of('\\');

  /* select the last one */
  int slash;
  if(slash2>slash1)
    slash = slash2;
  else
    slash = slash1;

  return strFileNameAndPath.substr(slash+1);
}

CStdString CUtil::GetTitleFromPath(const CStdString& strFileNameAndPath, bool bIsFolder /* = false */)
{
  // use above to get the filename
  CStdString path(strFileNameAndPath);
  RemoveSlashAtEnd(path);
  CStdString strFilename = GetFileName(path);

  CURL url(strFileNameAndPath);
  CStdString strHostname = url.GetHostName();

#ifdef HAS_UPNP
  // UPNP
  if (url.GetProtocol() == "upnp")
    strFilename = CUPnPDirectory::GetFriendlyName(strFileNameAndPath.c_str());
#endif

  if (url.GetProtocol() == "rss")
  {
    url.SetProtocol("http");
    path = url.Get();
    CRssFeed feed;
    feed.Init(path);
    feed.ReadFeed();
    strFilename = feed.GetFeedTitle();
  }

  // LastFM
  if (url.GetProtocol() == "lastfm")
  {
    if (strFilename.IsEmpty())
      strFilename = g_localizeStrings.Get(15200);
    else
      strFilename = g_localizeStrings.Get(15200) + " - " + strFilename;
  }

  // Shoutcast
  else if (url.GetProtocol() == "shout")
  {
    const int genre = strFileNameAndPath.find_first_of('=');
    if(genre <0)
      strFilename = g_localizeStrings.Get(260);
    else
      strFilename = g_localizeStrings.Get(260) + " - " + strFileNameAndPath.substr(genre+1).c_str();
  }

  // Windows SMB Network (SMB)
  else if (url.GetProtocol() == "smb" && strFilename.IsEmpty())
    strFilename = g_localizeStrings.Get(20171);

  // XBMSP Network
  else if (url.GetProtocol() == "xbms" && strFilename.IsEmpty())
    strFilename = "XBMSP Network";

  // iTunes music share (DAAP)
  else if (url.GetProtocol() == "daap" && strFilename.IsEmpty())
    strFilename = g_localizeStrings.Get(20174);

  // HDHomerun Devices
  else if (url.GetProtocol() == "hdhomerun" && strFilename.IsEmpty())
    strFilename = "HDHomerun Devices";

  // ReplayTV Devices
  else if (url.GetProtocol() == "rtv")
    strFilename = "ReplayTV Devices";

  // SAP Streams
  else if (url.GetProtocol() == "sap" && strFilename.IsEmpty())
    strFilename = "SAP Streams";

  // Music Playlists
  else if (path.Left(24).Equals("special://musicplaylists"))
    strFilename = g_localizeStrings.Get(20011);

  // Video Playlists
  else if (path.Left(24).Equals("special://videoplaylists"))
    strFilename = g_localizeStrings.Get(20012);

  // now remove the extension if needed
  if (g_guiSettings.GetBool("filelists.hideextensions") && !bIsFolder)
  {
    RemoveExtension(strFilename);
    return strFilename;
  }
  return strFilename;
}

bool CUtil::GetVolumeFromFileName(const CStdString& strFileName, CStdString& strFileTitle, CStdString& strVolumeNumber)
{
  const CStdStringArray &regexps = g_advancedSettings.m_videoStackRegExps;

  CStdString strFileNameTemp = strFileName;
  CStdString strFileNameLower = strFileName;
  strFileNameLower.MakeLower();

  CStdString strVolume;
  CStdString strTestString;
  CRegExp reg;

//  CLog::Log(LOGDEBUG, "GetVolumeFromFileName:[%s]", strFileNameLower.c_str());
  for (unsigned int i = 0; i < regexps.size(); i++)
  {
    CStdString strRegExp = regexps[i];
    if (!reg.RegComp(strRegExp.c_str()))
    { // invalid regexp - complain in logs
      CLog::Log(LOGERROR, "Invalid RegExp:[%s]", regexps[i].c_str());
      continue;
    }
//    CLog::Log(LOGDEBUG, "Regexp:[%s]", regexps[i].c_str());

    int iFoundToken = reg.RegFind(strFileNameLower.c_str());
    if (iFoundToken >= 0)
    {
      int iRegLength = reg.GetFindLen();
      int iCount = reg.GetSubCount();

      /*
      reg.DumpOvector(LOGDEBUG);
      CLog::Log(LOGDEBUG, "Subcount=%i", iCount);
      for (int j = 0; j <= iCount; j++)
      {
        CStdString str = reg.GetMatch(j);
        CLog::Log(LOGDEBUG, "Sub(%i):[%s]", j, str.c_str());
      }
      */

      // simple regexp, only the volume is captured
      if (iCount == 1)
      {
        strVolumeNumber = reg.GetMatch(1);
        if (strVolumeNumber.IsEmpty()) return false;

        // Remove the extension (if any).  We do this on the base filename, as the regexp
        // match may include some of the extension (eg the "." in particular).
        // The extension will then be added back on at the end - there is no reason
        // to clean it off here. It will be cleaned off during the display routine, if
        // the settings to hide extensions are turned on.
        CStdString strFileNoExt = strFileNameTemp;
        RemoveExtension(strFileNoExt);
        CStdString strFileExt = strFileNameTemp.Right(strFileNameTemp.length() - strFileNoExt.length());
        CStdString strFileRight = strFileNoExt.Mid(iFoundToken + iRegLength);
        strFileTitle = strFileName.Left(iFoundToken) + strFileRight + strFileExt;

        return true;
      }

      // advanced regexp with prefix (1), volume (2), and suffix (3)
      else if (iCount == 3)
      {
        // second subpatten contains the stacking volume
        strVolumeNumber = reg.GetMatch(2);
        if (strVolumeNumber.IsEmpty()) return false;

        // everything before the regexp match
        strFileTitle = strFileName.Left(iFoundToken);

        // first subpattern contains prefix
        strFileTitle += reg.GetMatch(1);

        // third subpattern contains suffix
        strFileTitle += reg.GetMatch(3);

        // everything after the regexp match
        strFileTitle += strFileNameTemp.Mid(iFoundToken + iRegLength);

        return true;
      }

      // unknown regexp format
      else
      {
        CLog::Log(LOGERROR, "Incorrect movie stacking regexp format:[%s]", regexps[i].c_str());
      }
    }
  }
  return false;
}

void CUtil::RemoveExtension(CStdString& strFileName)
{
  if(strFileName.Find("://") >= 0)
  {
    CURL url(strFileName);
    strFileName = url.GetFileName();
    RemoveExtension(strFileName);
    url.SetFileName(strFileName);
    strFileName = url.Get();
    return;
  }

  int iPos = strFileName.ReverseFind(".");
  // Extension found
  if (iPos > 0)
  {
    CStdString strExtension;
    CUtil::GetExtension(strFileName, strExtension);
    strExtension.ToLower();

    CStdString strFileMask;
    strFileMask = g_stSettings.m_pictureExtensions;
    strFileMask += g_stSettings.m_musicExtensions;
    strFileMask += g_stSettings.m_videoExtensions;
#if defined(__APPLE__)
    strFileMask += ".py|.xml|.milk|.xpr|.cdg|.app|.applescript|.workflow";
#else
    strFileMask += ".py|.xml|.milk|.xpr|.cdg";
#endif

    // Only remove if its a valid media extension
    if (strFileMask.Find(strExtension.c_str()) >= 0)
      strFileName = strFileName.Left(iPos);
  }
}

void CUtil::CleanString(CStdString& strFileName, CStdString& strTitle, CStdString& strTitleAndYear, CStdString& strYear, bool bIsFolder /* = false */)
{

  strTitleAndYear = strFileName;

  if (strFileName.Equals(".."))
   return;

  const CStdStringArray &regexps = g_advancedSettings.m_videoCleanStringRegExps;

  CRegExp reTags, reYear;
  CStdString strExtension;
  GetExtension(strFileName, strExtension);

  if (!reYear.RegComp(g_advancedSettings.m_videoCleanDateTimeRegExp))
  {
    CLog::Log(LOGERROR, "%s: Invalid datetime clean RegExp:'%s'", __FUNCTION__, g_advancedSettings.m_videoCleanDateTimeRegExp.c_str());
  }
  else
  {
    if (reYear.RegFind(strTitleAndYear.c_str()) >= 0)
    {
      strTitleAndYear = reYear.GetReplaceString("\\1");
      strYear = reYear.GetReplaceString("\\2");
    }
  }

  RemoveExtension(strTitleAndYear);

  for (unsigned int i = 0; i < regexps.size(); i++)
  {
    if (!reTags.RegComp(regexps[i].c_str()))
    { // invalid regexp - complain in logs
      CLog::Log(LOGERROR, "%s: Invalid string clean RegExp:'%s'", __FUNCTION__, regexps[i].c_str());
      continue;
    }
    int j=0;
    if ((j=reTags.RegFind(strFileName.ToLower().c_str())) > 0)
      strTitleAndYear = strTitleAndYear.Mid(0, j);
  }

  // final cleanup - special characters used instead of spaces:
  // all '_' tokens should be replaced by spaces
  // if the file contains no spaces, all '.' tokens should be replaced by
  // spaces - one possibility of a mistake here could be something like:
  // "Dr..StrangeLove" - hopefully no one would have anything like this.
  {
    bool initialDots = true;
    bool alreadyContainsSpace = (strTitleAndYear.Find(' ') >= 0);

    for (int i = 0; i < (int)strTitleAndYear.size(); i++)
    {
      char c = strTitleAndYear.GetAt(i);

      if (c != '.')
        initialDots = false;

      if ((c == '_') || ((!alreadyContainsSpace) && !initialDots && (c == '.')))
      {
        strTitleAndYear.SetAt(i, ' ');
      }
    }
  }

  strTitle = strTitleAndYear.Trim();

  // append year
  if (!strYear.IsEmpty())
    strTitleAndYear = strTitle + " (" + strYear + ")";

  // restore extension if needed
  if (!g_guiSettings.GetBool("filelists.hideextensions") && !bIsFolder)
    strTitleAndYear += strExtension;

}

void CUtil::GetCommonPath(CStdString& strParent, const CStdString& strPath)
{
  // find the common path of parent and path
  unsigned int j = 1;
  while (j <= min(strParent.size(), strPath.size()) && strnicmp(strParent.c_str(), strPath.c_str(), j) == 0)
    j++;
  strParent = strParent.Left(j - 1);
  // they should at least share a / at the end, though for things such as path/cd1 and path/cd2 there won't be
  if (!CUtil::HasSlashAtEnd(strParent))
  {
    // currently GetDirectory() removes trailing slashes
    CUtil::GetDirectory(strParent.Mid(0), strParent);
    CUtil::AddSlashAtEnd(strParent);
  }
}

CStdString CUtil::GetParentPath(const CStdString& strPath)
{
  CStdString strReturn;
  GetParentPath(strPath, strReturn);
  return strReturn;
}

bool CUtil::GetParentPath(const CStdString& strPath, CStdString& strParent)
{
  strParent = "";

  CURL url(strPath);
  CStdString strFile = url.GetFileName();
  if ( ((url.GetProtocol() == "rar") || (url.GetProtocol() == "zip")) && strFile.IsEmpty())
  {
    strFile = url.GetHostName();
    return GetParentPath(strFile, strParent);
  }
  else if (url.GetProtocol() == "stack")
  {
    CStackDirectory dir;
    CFileItemList items;
    dir.GetDirectory(strPath,items);
    CUtil::GetDirectory(items[0]->m_strPath,items[0]->m_strDVDLabel);
    if (items[0]->m_strDVDLabel.Mid(0,6).Equals("rar://") || items[0]->m_strDVDLabel.Mid(0,6).Equals("zip://"))
      GetParentPath(items[0]->m_strDVDLabel, strParent);
    else
      strParent = items[0]->m_strDVDLabel;
    for( int i=1;i<items.Size();++i)
    {
      CUtil::GetDirectory(items[i]->m_strPath,items[i]->m_strDVDLabel);
      if (items[0]->m_strDVDLabel.Mid(0,6).Equals("rar://") || items[0]->m_strDVDLabel.Mid(0,6).Equals("zip://"))
        GetParentPath(items[i]->m_strDVDLabel, items[i]->m_strPath);
      else
        items[i]->m_strPath = items[i]->m_strDVDLabel;

      GetCommonPath(strParent,items[i]->m_strPath);
    }
    return true;
  }
  else if (url.GetProtocol() == "multipath")
  {
    // get the parent path of the first item
    return GetParentPath(CMultiPathDirectory::GetFirstPath(strPath), strParent);
  }
  else if (url.GetProtocol() == "plugin")
  {
    if (!url.GetOptions().IsEmpty())
    {
      url.SetOptions("");
      strParent = url.Get();
      return true;
    }
    if (!url.GetFileName().IsEmpty())
    {
      url.SetFileName("");
      strParent = url.Get();
      return true;
    }
    if (!url.GetHostName().IsEmpty())
    {
      url.SetHostName("");
      strParent = url.Get();
      return true;
    }
    return true;  // already at root
  }
  else if (url.GetProtocol() == "special")
  {
    if (HasSlashAtEnd(strFile) )
      strFile = strFile.Left(strFile.size() - 1);
    if(strFile.ReverseFind('/') < 0)
      return false;
  }
  else if (strFile.size() == 0)
  {
    if (url.GetHostName().size() > 0)
    {
      // we have an share with only server or workgroup name
      // set hostname to "" and return true to get back to root
      url.SetHostName("");
      strParent = url.Get();
      return true;
    }
    return false;
  }

  if (HasSlashAtEnd(strFile) )
  {
    strFile = strFile.Left(strFile.size() - 1);
  }

  int iPos = strFile.ReverseFind('/');
#ifndef _LINUX
  if (iPos < 0)
  {
    iPos = strFile.ReverseFind('\\');
  }
#endif
  if (iPos < 0)
  {
    url.SetFileName("");
    strParent = url.Get();
    return true;
  }

  strFile = strFile.Left(iPos);

  CUtil::AddSlashAtEnd(strFile);

  url.SetFileName(strFile);
  strParent = url.Get();
  return true;
}

void CUtil::GetQualifiedFilename(const CStdString &strBasePath, CStdString &strFilename)
{
  //Make sure you have a full path in the filename, otherwise adds the base path before.
  CURL plItemUrl(strFilename);
  CURL plBaseUrl(strBasePath);
  int iDotDotLoc, iBeginCut, iEndCut;

  if (plBaseUrl.IsLocal()) //Base in local directory
  {
    if (plItemUrl.IsLocal() ) //Filename is local or not qualified
    {
#ifdef _LINUX
      if (!( (strFilename.c_str()[1] == ':') || (strFilename.c_str()[0] == '/') ) ) //Filename not fully qualified
#else
      if (!( strFilename.c_str()[1] == ':')) //Filename not fully qualified
#endif
      {
        if (strFilename.c_str()[0] == '/' || strFilename.c_str()[0] == '\\' || HasSlashAtEnd(strBasePath))
        {
          strFilename = strBasePath + strFilename;
          strFilename.Replace('/', '\\');
        }
        else
        {
          strFilename = strBasePath + '\\' + strFilename;
          strFilename.Replace('/', '\\');
        }
      }
    }
    strFilename.Replace("\\.\\", "\\");
    while ((iDotDotLoc = strFilename.Find("\\..\\")) > 0)
    {
      iEndCut = iDotDotLoc + 4;
      iBeginCut = strFilename.Left(iDotDotLoc).ReverseFind('\\') + 1;
      strFilename.Delete(iBeginCut, iEndCut - iBeginCut);
    }
  }
  else //Base is remote
  {
    if (plItemUrl.IsLocal()) //Filename is local
    {
#ifdef _LINUX
      if ( (strFilename.c_str()[1] == ':') || (strFilename.c_str()[0] == '/') )  //Filename not fully qualified
#else
      if (strFilename[1] == ':') // already fully qualified
#endif
        return;
      if (strFilename.c_str()[0] == '/' || strFilename.c_str()[0] == '\\' || HasSlashAtEnd(strBasePath)) //Begins with a slash.. not good.. but we try to make the best of it..

      {
        strFilename = strBasePath + strFilename;
        strFilename.Replace('\\', '/');
      }
      else
      {
        strFilename = strBasePath + '/' + strFilename;
        strFilename.Replace('\\', '/');
      }
    }
    strFilename.Replace("/./", "/");
    while ((iDotDotLoc = strFilename.Find("/../")) > 0)
    {
      iEndCut = iDotDotLoc + 4;
      iBeginCut = strFilename.Left(iDotDotLoc).ReverseFind('/') + 1;
      strFilename.Delete(iBeginCut, iEndCut - iBeginCut);
    }
  }
}

void CUtil::RunShortcut(const char* szShortcutPath)
{
}

void CUtil::GetHomePath(CStdString& strPath)
{
  char szXBEFileName[1024];
  CIoSupport::GetXbePath(szXBEFileName);
  strPath = getenv("XBMC_HOME");
  if (strPath != NULL && !strPath.IsEmpty())
  {
#ifdef _WIN32
    //expand potential relative path to full path
    if(GetFullPathName(strPath, 1024, szXBEFileName, 0) != 0)
    {
      strPath = szXBEFileName;
    }
#endif
  }
  else
  {
#ifdef __APPLE__
    int      result = -1;
    char     given_path[2*MAXPATHLEN];
    uint32_t path_size = 2*MAXPATHLEN;

    result = _NSGetExecutablePath(given_path, &path_size);
    if (result == 0)
    {
      // Move backwards to last /.
      for (int n=strlen(given_path)-1; given_path[n] != '/'; n--)
        given_path[n] = '\0';

      // Assume local path inside application bundle.
      strcat(given_path, "../Resources/XBMC/");

      // Convert to real path.
      char real_path[2*MAXPATHLEN];
      if (realpath(given_path, real_path) != NULL)
      {
        strPath = real_path;
        return;
      }
    }
#endif
    char *szFileName = strrchr(szXBEFileName, PATH_SEPARATOR_CHAR);
    *szFileName = 0;
    strPath = szXBEFileName;
  }
}

void CUtil::ReplaceExtension(const CStdString& strFile, const CStdString& strNewExtension, CStdString& strChangedFile)
{
  if(strFile.Find("://") >= 0)
  {
    CURL url(strFile);
    ReplaceExtension(url.GetFileName(), strNewExtension, strChangedFile);
    url.SetFileName(strChangedFile);
    strChangedFile = url.Get();
    return;
  }

  CStdString strExtension;
  GetExtension(strFile, strExtension);
  if ( strExtension.size() )
  {
    strChangedFile = strFile.substr(0, strFile.size() - strExtension.size()) ;
    strChangedFile += strNewExtension;
  }
  else
  {
    strChangedFile = strFile;
    strChangedFile += strNewExtension;
  }
}

bool CUtil::HasSlashAtEnd(const CStdString& strFile)
{
  if (strFile.size() == 0) return false;
  char kar = strFile.c_str()[strFile.size() - 1];

  if (kar == '/' || kar == '\\')
    return true;

  return false;
}

bool CUtil::IsRemote(const CStdString& strFile)
{
  if (IsCDDA(strFile) || IsISO9660(strFile) || IsPlugin(strFile))
    return false;

  if (IsSpecial(strFile))
    return IsRemote(CSpecialProtocol::TranslatePath(strFile));

  if(IsStack(strFile))
    return IsRemote(CStackDirectory::GetFirstStackedFile(strFile));

  if (IsVirtualPath(strFile))
  { // virtual paths need to be checked separately
    CVirtualPathDirectory dir;
    vector<CStdString> paths;
    if (dir.GetPathes(strFile, paths))
    {
      for (unsigned int i = 0; i < paths.size(); i++)
        if (IsRemote(paths[i])) return true;
    }
    return false;
  }

  if(IsMultiPath(strFile))
  { // virtual paths need to be checked separately
    vector<CStdString> paths;
    if (CMultiPathDirectory::GetPaths(strFile, paths))
    {
      for (unsigned int i = 0; i < paths.size(); i++)
        if (IsRemote(paths[i])) return true;
    }
    return false;
  }

  CURL url(strFile);
  if(IsInArchive(strFile))
    return IsRemote(url.GetHostName());

  if (!url.IsLocal())
    return true;

  return false;
}

bool CUtil::IsOnDVD(const CStdString& strFile)
{
#ifdef _WIN32
  if (strFile.Mid(1,1) == ":")
    return (GetDriveType(strFile.Left(2)) == DRIVE_CDROM);
#else
  if (strFile.Left(2).CompareNoCase("d:") == 0)
    return true;
#endif

  if (strFile.Left(4).CompareNoCase("dvd:") == 0)
    return true;

  if (strFile.Left(4).CompareNoCase("udf:") == 0)
    return true;

  if (strFile.Left(8).CompareNoCase("iso9660:") == 0)
    return true;

  if (strFile.Left(5).CompareNoCase("cdda:") == 0)
    return true;

  return false;
}

bool CUtil::IsOnLAN(const CStdString& strPath)
{
  if(IsMultiPath(strPath))
    return CUtil::IsOnLAN(CMultiPathDirectory::GetFirstPath(strPath));

  if(IsStack(strPath))
    return CUtil::IsOnLAN(CStackDirectory::GetFirstStackedFile(strPath));

  if(IsSpecial(strPath))
    return CUtil::IsOnLAN(CSpecialProtocol::TranslatePath(strPath));

  if(IsDAAP(strPath))
    return true;

  if(IsTuxBox(strPath))
    return true;

  if(IsUPnP(strPath))
    return true;

  CURL url(strPath);
  if(IsInArchive(strPath))
    return CUtil::IsOnLAN(url.GetHostName());

  if(!IsRemote(strPath))
    return false;

  CStdString host = url.GetHostName();
  if(host.length() == 0)
    return false;


  unsigned long address = ntohl(inet_addr(host.c_str()));
  if(address == INADDR_NONE)
  {
    CStdString ip;
    if(CDNSNameCache::Lookup(host, ip))
      address = ntohl(inet_addr(ip.c_str()));
  }

  if(address == INADDR_NONE)
  {
    // assume a hostname without dot's
    // is local (smb netbios hostnames)
    if(host.find('.') == string::npos)
      return true;
  }
  else
  {
    // check if we are on the local subnet
    if (!g_application.getNetwork().GetFirstConnectedInterface())
      return false;
    unsigned long subnet = ntohl(inet_addr(g_application.getNetwork().GetFirstConnectedInterface()->GetCurrentNetmask()));
    unsigned long local  = ntohl(inet_addr(g_application.getNetwork().GetFirstConnectedInterface()->GetCurrentIPAddress()));
    if( (address & subnet) == (local & subnet) )
      return true;
  }
  return false;
}

bool CUtil::IsMultiPath(const CStdString& strPath)
{
  CURL url(strPath);

  return url.GetProtocol() == "multipath";
}

bool CUtil::IsDVD(const CStdString& strFile)
{
  CStdString strFileLow = strFile;
  strFileLow.MakeLower();
#if defined(_WIN32)
  if((GetDriveType(strFile.c_str()) == DRIVE_CDROM) || strFile.Left(6).Equals("dvd://"))
    return true;
#else
  if (strFileLow == "d:/"  || strFileLow == "d:\\"  || strFileLow == "d:" || strFileLow == "iso9660://" || strFileLow == "udf://" || strFileLow == "dvd://1" )
    return true;
#endif

  return false;
}

bool CUtil::IsVirtualPath(const CStdString& strFile)
{
  CURL url(strFile);

  return url.GetProtocol() == "virtualpath";
}

bool CUtil::IsStack(const CStdString& strFile)
{
  CURL url(strFile);

  return url.GetProtocol() == "stack";
}

bool CUtil::IsRAR(const CStdString& strFile)
{
  CStdString strExtension;
  CUtil::GetExtension(strFile,strExtension);

  if (strExtension.Equals(".001") && strFile.Mid(strFile.length()-7,7).CompareNoCase(".ts.001"))
    return true;
  
  if (strExtension.CompareNoCase(".cbr") == 0)
    return true;
  
  if (strExtension.CompareNoCase(".rar") == 0)
    return true;

  return false;
}

bool CUtil::IsInArchive(const CStdString &strFile)
{
  return IsInZIP(strFile) || IsInRAR(strFile);
}

bool CUtil::IsInZIP(const CStdString& strFile)
{
  CURL url(strFile);

  return url.GetProtocol() == "zip" && url.GetFileName() != "";
}

bool CUtil::IsInRAR(const CStdString& strFile)
{
  CURL url(strFile);

  return url.GetProtocol() == "rar" && url.GetFileName() != "";
}

bool CUtil::IsZIP(const CStdString& strFile) // also checks for comic books!
{
  CStdString strExtension;
  CUtil::GetExtension(strFile,strExtension);

  if (strExtension.CompareNoCase(".zip") == 0)
    return true;

  if (strExtension.CompareNoCase(".cbz") == 0)
    return true;

  return false;
}

bool CUtil::IsSpecial(const CStdString& strFile)
{
  CStdString strFile2(strFile);

  if (IsStack(strFile))
    strFile2 = CStackDirectory::GetFirstStackedFile(strFile);

  CURL url(strFile2);

  return url.GetProtocol().Equals("special");
}

bool CUtil::IsPlugin(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("plugin") && !url.GetFileName().IsEmpty();
}

bool CUtil::IsPluginRoot(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("plugin") && url.GetFileName().IsEmpty();
}

bool CUtil::IsCDDA(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("cdda") && !url.GetFileName().IsEmpty();
}

bool CUtil::IsISO9660(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("iso9660");
}

bool CUtil::IsSmb(const CStdString& strFile)
{
  CStdString strFile2(strFile);

  if (IsStack(strFile))
    strFile2 = CStackDirectory::GetFirstStackedFile(strFile);

  CURL url(strFile2);

  return url.GetProtocol().Equals("smb");
}

bool CUtil::IsFTP(const CStdString& strFile)
{
  CStdString strFile2(strFile);

  if (IsStack(strFile))
    strFile2 = CStackDirectory::GetFirstStackedFile(strFile);

  CURL url(strFile2);

  return url.GetProtocol() == "ftp"  ||
         url.GetProtocol() == "ftpx" ||
         url.GetProtocol() == "ftps";
}

bool CUtil::IsDAAP(const CStdString& strFile)
{
  return strFile.Left(5).Equals("daap:");
}

bool CUtil::IsUPnP(const CStdString& strFile)
{
  return strFile.Left(5).Equals("upnp:");
}

bool CUtil::IsTuxBox(const CStdString& strFile)
{
  return strFile.Left(7).Equals("tuxbox:");
}

bool CUtil::IsMythTV(const CStdString& strFile)
{
  return strFile.Left(5).Equals("myth:");
}

bool CUtil::IsHDHomeRun(const CStdString& strFile)
{
  return strFile.Left(10).Equals("hdhomerun:");
}

bool CUtil::IsVTP(const CStdString& strFile)
{
  return strFile.Left(4).Equals("vtp:");
}

bool CUtil::IsHTSP(const CStdString& strFile)
{
  return strFile.Left(5).Equals("htsp:");
}

bool CUtil::IsLiveTV(const CStdString& strFile)
{
  if (IsTuxBox(strFile) || IsVTP(strFile) || IsHDHomeRun(strFile) || IsHTSP(strFile))
    return true;

  if (IsMythTV(strFile) && CCMythDirectory::IsLiveTV(strFile))
    return true;

  return false;
}

bool CUtil::IsMusicDb(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("musicdb");
}

bool CUtil::IsVideoDb(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("videodb");
}

bool CUtil::IsShoutCast(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("shout");
}

bool CUtil::IsLastFM(const CStdString& strFile)
{
  CURL url(strFile);
  return url.GetProtocol().Equals("lastfm");
}

bool CUtil::ExcludeFileOrFolder(const CStdString& strFileOrFolder, const CStdStringArray& regexps)
{
  if (strFileOrFolder.IsEmpty())
    return false;

  CStdString strExclude = strFileOrFolder;
  RemoveSlashAtEnd(strExclude);
  strExclude = GetFileName(strExclude);
  strExclude.MakeLower();

  CRegExp regExExcludes;

  for (unsigned int i = 0; i < regexps.size(); i++)
  {
    if (!regExExcludes.RegComp(regexps[i].c_str()))
    { // invalid regexp - complain in logs
      CLog::Log(LOGERROR, "%s: Invalid exclude RegExp:'%s'", __FUNCTION__, regexps[i].c_str());
      continue;
    }
    if (regExExcludes.RegFind(strExclude) > -1)
    {
      CLog::Log(LOGDEBUG, "%s: File '%s' excluded. (Matches exclude rule RegExp:'%s')", __FUNCTION__, strFileOrFolder.c_str(), regexps[i].c_str());
      return true;
    }
  }
  return false;
}

void CUtil::GetFileAndProtocol(const CStdString& strURL, CStdString& strDir)
{
  strDir = strURL;
  if (!IsRemote(strURL)) return ;
  if (IsDVD(strURL)) return ;

  CURL url(strURL);
  strDir.Format("%s://%s", url.GetProtocol().c_str(), url.GetFileName().c_str());
}

int CUtil::GetDVDIfoTitle(const CStdString& strFile)
{
  CStdString strFilename = GetFileName(strFile);
  if (strFilename.Equals("video_ts.ifo")) return 0;
  //VTS_[TITLE]_0.IFO
  return atoi(strFilename.Mid(4, 2).c_str());
}

void CUtil::UrlDecode(CStdString& strURLData)
//modified to be more accomodating - if a non hex value follows a % take the characters directly and don't raise an error.
// However % characters should really be escaped like any other non safe character (www.rfc-editor.org/rfc/rfc1738.txt)
{
  CStdString strResult;

  /* result will always be less than source */
  strResult.reserve( strURLData.length() );

  for (unsigned int i = 0; i < strURLData.size(); ++i)
  {
    int kar = (unsigned char)strURLData[i];
    if (kar == '+') strResult += ' ';
    else if (kar == '%')
    {
      if (i < strURLData.size() - 2)
      {
        CStdString strTmp;
        strTmp.assign(strURLData.substr(i + 1, 2));
        int dec_num=-1;
        sscanf(strTmp,"%x",(unsigned int *)&dec_num);
        if (dec_num<0 || dec_num>255)
          strResult += kar;
        else
        {
          strResult += (char)dec_num;
          i += 2;
        }
      }
      else
        strResult += kar;
    }
    else strResult += kar;
  }
  strURLData = strResult;
}

void CUtil::URLEncode(CStdString& strURLData)
{
  CStdString strResult;

  /* wonder what a good value is here is, depends on how often it occurs */
  strResult.reserve( strURLData.length() * 2 );

  for (int i = 0; i < (int)strURLData.size(); ++i)
  {
    int kar = (unsigned char)strURLData[i];
    //if (kar == ' ') strResult += '+';
    if (isalnum(kar)) strResult += kar;
    else
    {
      CStdString strTmp;
      strTmp.Format("%%%02.2x", kar);
      strResult += strTmp;
    }
  }
  strURLData = strResult;
}

bool CUtil::GetDirectoryName(const CStdString& strFileName, CStdString& strDescription)
{
  CStdString strFName = CUtil::GetFileName(strFileName);
  strDescription = strFileName.Left(strFileName.size() - strFName.size());
  CUtil::RemoveSlashAtEnd(strDescription);

  int iPos = strDescription.ReverseFind("\\");
  if (iPos < 0)
    iPos = strDescription.ReverseFind("/");
  if (iPos >= 0)
  {
    CStdString strTmp = strDescription.Right(strDescription.size()-iPos-1);
    strDescription = strTmp;//strDescription.Right(strDescription.size() - iPos - 1);
  }
  else if (strDescription.size() <= 0)
    strDescription = strFName;
  return true;
}

void CUtil::GetDVDDriveIcon( const CStdString& strPath, CStdString& strIcon )
{
  if ( !g_mediaManager.IsDiscInDrive() )
  {
    strIcon = "DefaultDVDEmpty.png";
    return ;
  }

  if ( IsDVD(strPath) )
  {
#ifdef HAS_DVD_DRIVE
    CCdInfo* pInfo = g_mediaManager.GetCdInfo();
    //  xbox DVD
    if ( pInfo != NULL && pInfo->IsUDFX( 1 ) )
    {
      strIcon = "DefaultXboxDVD.png";
      return ;
    }
#endif    
    strIcon = "DefaultDVDRom.png";
    return ;
  }

  if ( IsISO9660(strPath) )
  {
#ifdef HAS_DVD_DRIVE    
    CCdInfo* pInfo = g_mediaManager.GetCdInfo();
    if ( pInfo != NULL && pInfo->IsVideoCd( 1 ) )
    {
      strIcon = "DefaultVCD.png";
      return ;
    }
#endif    
    strIcon = "DefaultDVDRom.png";
    return ;
  }

  if ( IsCDDA(strPath) )
  {
    strIcon = "DefaultCDDA.png";
    return ;
  }
}

void CUtil::RemoveTempFiles()
{
  WIN32_FIND_DATA wfd;

  CStdString strAlbumDir = CUtil::AddFileToFolder(g_settings.GetDatabaseFolder(), "*.tmp");
  memset(&wfd, 0, sizeof(wfd));

  CAutoPtrFind hFind( FindFirstFile(_P(strAlbumDir).c_str(), &wfd));
  if (!hFind.isValid())
    return ;
  do
  {
    if ( !(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) )
      CFile::Delete(CUtil::AddFileToFolder(g_settings.GetDatabaseFolder(), wfd.cFileName));
  }
  while (FindNextFile(hFind, &wfd));
}

bool CUtil::IsHD(const CStdString& strFileName)
{
  CURL url(_P(strFileName));
  return url.IsLocal();
}

void CUtil::ClearSubtitles()
{
  //delete cached subs
  WIN32_FIND_DATA wfd;
#ifndef _LINUX
  CAutoPtrFind hFind ( FindFirstFile(_P("special://temp/*.*"), &wfd));
#else
  CAutoPtrFind hFind ( FindFirstFile(_P("special://temp/*"), &wfd));
#endif
  if (hFind.isValid())
  {
    do
    {
      if (wfd.cFileName[0] != 0)
      {
        if ( (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 )
        {
          CStdString strFile;
          strFile.Format("special://temp/%s", wfd.cFileName);
          if (strFile.Find("subtitle") >= 0 )
            CFile::Delete(strFile);
          else if (strFile.Find("vobsub_queue") >= 0 )
            CFile::Delete(strFile);
        }
      }
    }
    while (FindNextFile((HANDLE)hFind, &wfd));
  }
}

static const char * sub_exts[] = { ".utf", ".utf8", ".utf-8", ".sub", ".srt", ".smi", ".rt", ".txt", ".ssa", ".aqt", ".jss", ".ass", ".idx", NULL};

void CUtil::CacheSubtitles(const CStdString& strMovie, CStdString& strExtensionCached, XFILE::IFileCallback *pCallback )
{
  unsigned int startTimer = CTimeUtils::GetTimeMS();
  CLog::Log(LOGDEBUG,"%s: START", __FUNCTION__);

  // new array for commons sub dirs
  const char * common_sub_dirs[] = {"subs",
                              "Subs",
                              "subtitles",
                              "Subtitles",
                              "vobsubs",
                              "Vobsubs",
                              "sub",
                              "Sub",
                              "vobsub",
                              "Vobsub",
                              "subtitle",
                              "Subtitle",
                              NULL};

  vector<CStdString> vecExtensionsCached;
  strExtensionCached = "";

  ClearSubtitles();

  CFileItem item(strMovie, false);
  if (item.IsInternetStream()) return ;
  if (item.IsHDHomeRun()) return ;
  if (item.IsPlayList()) return ;
  if (!item.IsVideo()) return ;

  vector<CStdString> strLookInPaths;

  CStdString strFileName;
  CStdString strFileNameNoExt;
  CStdString strPath;

  CUtil::Split(strMovie, strPath, strFileName);
  ReplaceExtension(strFileName, "", strFileNameNoExt);
  strLookInPaths.push_back(strPath);

  if (!g_stSettings.iAdditionalSubtitleDirectoryChecked && !g_guiSettings.GetString("subtitles.custompath").IsEmpty()) // to avoid checking non-existent directories (network) every time..
  {
    if (!g_application.getNetwork().IsAvailable() && !IsHD(g_guiSettings.GetString("subtitles.custompath")))
    {
      CLog::Log(LOGINFO,"CUtil::CacheSubtitles: disabling alternate subtitle directory for this session, it's nonaccessible");
      g_stSettings.iAdditionalSubtitleDirectoryChecked = -1; // disabled
    }
    else if (!CDirectory::Exists(g_guiSettings.GetString("subtitles.custompath")))
    {
      CLog::Log(LOGINFO,"CUtil::CacheSubtitles: disabling alternate subtitle directory for this session, it's nonexistant");
      g_stSettings.iAdditionalSubtitleDirectoryChecked = -1; // disabled
    }

    g_stSettings.iAdditionalSubtitleDirectoryChecked = 1;
  }

  if (strMovie.substr(0,6) == "rar://") // <--- if this is found in main path then ignore it!
  {
    CURL url(strMovie);
    CStdString strArchive = url.GetHostName();
    CUtil::Split(strArchive, strPath, strFileName);
    strLookInPaths.push_back(strPath);
  }

  // checking if any of the common subdirs exist ..
  CLog::Log(LOGDEBUG,"%s: Checking for common subirs...", __FUNCTION__);

  vector<CStdString> token;
  Tokenize(strPath,token,"/\\");
  if (token[token.size()-1].size() == 3 && token[token.size()-1].Mid(0,2).Equals("cd"))
  {
    CStdString strPath2;
    GetParentPath(strPath,strPath2);
    strLookInPaths.push_back(strPath2);
  } 
  int iSize = strLookInPaths.size();
  for (int i=0;i<iSize;++i)
  {
    for (int j=0; common_sub_dirs[j]; j++)
    {
      CStdString strPath2;
      CUtil::AddFileToFolder(strLookInPaths[i],common_sub_dirs[j],strPath2);
      if (CDirectory::Exists(strPath2))
        strLookInPaths.push_back(strPath2);
    }
  }
  // .. done checking for common subdirs

  // check if there any cd-directories in the paths we have added so far
  char temp[6];
  iSize = strLookInPaths.size();
  for (int i=0;i<9;++i) // 9 cd's
  {
    sprintf(temp,"cd%i",i+1);
    for (int i=0;i<iSize;++i)
    {
      CStdString strPath2;
      CUtil::AddFileToFolder(strLookInPaths[i],temp,strPath2);
      if (CDirectory::Exists(strPath2))
        strLookInPaths.push_back(strPath2);
    }
  }
  // .. done checking for cd-dirs

  // this is last because we dont want to check any common subdirs or cd-dirs in the alternate <subtitles> dir.
  if (g_stSettings.iAdditionalSubtitleDirectoryChecked == 1)
  {
    strPath = g_guiSettings.GetString("subtitles.custompath");
    if (!HasSlashAtEnd(strPath))
      strPath += "/"; //Should work for both remote and local files
    strLookInPaths.push_back(strPath);
  }

  unsigned int nextTimer = CTimeUtils::GetTimeMS();
  CLog::Log(LOGDEBUG,"%s: Done (time: %i ms)", __FUNCTION__, (int)(nextTimer - startTimer));

  CStdString strLExt;
  CStdString strDest;
  CStdString strItem;

  // 2 steps for movie directory and alternate subtitles directory
  CLog::Log(LOGDEBUG,"%s: Searching for subtitles...", __FUNCTION__);
  for (unsigned int step = 0; step < strLookInPaths.size(); step++)
  {
    if (strLookInPaths[step].length() != 0)
    {
      CFileItemList items;

      CDirectory::GetDirectory(strLookInPaths[step], items,".utf|.utf8|.utf-8|.sub|.srt|.smi|.rt|.txt|.ssa|.text|.ssa|.aqt|.jss|.ass|.idx|.ifo|.rar|.zip",false);
      int fnl = strFileNameNoExt.size();

      CStdString strFileNameNoExtNoCase(strFileNameNoExt);
      strFileNameNoExtNoCase.MakeLower();
      for (int j = 0; j < (int)items.Size(); j++)
      {
        Split(items[j]->m_strPath, strPath, strItem);

        // is this a rar-file ..
        if ((CUtil::IsRAR(strItem) || CUtil::IsZIP(strItem)))
        {
          CStdString strRar, strItemWithPath;
          CUtil::AddFileToFolder(strLookInPaths[step],strFileNameNoExt+CUtil::GetExtension(strItem),strRar);
          CUtil::AddFileToFolder(strLookInPaths[step],strItem,strItemWithPath);

          unsigned int iPos = strMovie.substr(0,6)=="rar://"?1:0;
          iPos = strMovie.substr(0,6)=="zip://"?1:0;
          if ((step != iPos) || (strFileNameNoExtNoCase+".rar").Equals(strItem) || (strFileNameNoExtNoCase+".zip").Equals(strItem))
            CacheRarSubtitles( vecExtensionsCached, items[j]->m_strPath, strFileNameNoExtNoCase);
        }
        else
        {
          for (int i = 0; sub_exts[i]; i++)
          {
            int l = strlen(sub_exts[i]);

            //Cache any alternate subtitles.
            if (strItem.Left(9).ToLower() == "subtitle." && strItem.Right(l).ToLower() == sub_exts[i])
            {
              strLExt = strItem.Right(strItem.GetLength() - 9);
              strDest.Format("special://temp/subtitle.alt-%s", strLExt);
              if (CFile::Cache(items[j]->m_strPath, strDest, pCallback, NULL))
              {
                CLog::Log(LOGINFO, " cached subtitle %s->%s\n", strItem.c_str(), strDest.c_str());
                strExtensionCached = strLExt;
              }
            }

            //Cache subtitle with same name as movie
            if (strItem.Right(l).ToLower() == sub_exts[i] && strItem.Left(fnl).ToLower() == strFileNameNoExt.ToLower())
            {
              strLExt = strItem.Right(strItem.size() - fnl);
              strDest.Format("special://temp/subtitle%s", strLExt);
              if (find(vecExtensionsCached.begin(),vecExtensionsCached.end(),strLExt) == vecExtensionsCached.end())
              {
                if (CFile::Cache(items[j]->m_strPath, strDest, pCallback, NULL))
                {
                  vecExtensionsCached.push_back(strLExt);
                  CLog::Log(LOGINFO, " cached subtitle %s->%s\n", strItem.c_str(), strDest.c_str());
                }
              }
            }
          }
        }
      }

      g_directoryCache.ClearDirectory(strLookInPaths[step]);
    }
  }
  CLog::Log(LOGDEBUG,"%s: Done (time: %i ms)", __FUNCTION__, (int)(CTimeUtils::GetTimeMS() - nextTimer));

  // construct string of added exts?
  for (vector<CStdString>::iterator it=vecExtensionsCached.begin(); it != vecExtensionsCached.end(); ++it)
    strExtensionCached += *it+" ";

  CLog::Log(LOGDEBUG,"%s: END (total time: %i ms)", __FUNCTION__, (int)(CTimeUtils::GetTimeMS() - startTimer));
}

bool CUtil::CacheRarSubtitles(vector<CStdString>& vecExtensionsCached, const CStdString& strRarPath, const CStdString& strCompare, const CStdString& strExtExt)
{
  bool bFoundSubs = false;
  CFileItemList ItemList;

  // zip only gets the root dir
  if (CUtil::GetExtension(strRarPath).Equals(".zip"))
  {
    CStdString strZipPath;
    CUtil::CreateArchivePath(strZipPath,"zip",strRarPath,"");
    if (!CDirectory::GetDirectory(strZipPath,ItemList,"",false))
      return false;
  }
  else
  {
    // get _ALL_files in the rar, even those located in subdirectories because we set the bMask to false.
    // so now we dont have to find any subdirs anymore, all files in the rar is checked.
    if( !g_RarManager.GetFilesInRar(ItemList, strRarPath, false, "") )
      return false;
  }
  for (int it= 0 ; it <ItemList.Size();++it)
  {
    CStdString strPathInRar = ItemList[it]->m_strPath;
    CStdString strExt = CUtil::GetExtension(strPathInRar);

    if (find(vecExtensionsCached.begin(),vecExtensionsCached.end(),strExt) != vecExtensionsCached.end())
      continue;

    CLog::Log(LOGDEBUG, "CacheRarSubs:: Found file %s", strPathInRar.c_str());
    // always check any embedded rar archives
    // checking for embedded rars, I moved this outside the sub_ext[] loop. We only need to check this once for each file.
    if (CUtil::IsRAR(strPathInRar) || CUtil::IsZIP(strPathInRar))
    {
      CStdString strExtAdded;
      CStdString strRarInRar;
      if (CUtil::GetExtension(strPathInRar).Equals(".rar"))
        CUtil::CreateArchivePath(strRarInRar, "rar", strRarPath, strPathInRar);
      else
        CUtil::CreateArchivePath(strRarInRar, "zip", strRarPath, strPathInRar);
      CacheRarSubtitles(vecExtensionsCached,strRarInRar,strCompare, strExtExt);
    }
    // done checking if this is a rar-in-rar

    int iPos=0;
    CStdString strFileName = CUtil::GetFileName(strPathInRar);
    CStdString strFileNameNoCase(strFileName);
    strFileNameNoCase.MakeLower();
    if (strFileNameNoCase.Find(strCompare) >= 0)
      while (sub_exts[iPos])
      {
        if (strExt.CompareNoCase(sub_exts[iPos]) == 0)
        {
          CStdString strSourceUrl, strDestUrl;
          if (CUtil::GetExtension(strRarPath).Equals(".rar"))
            CUtil::CreateArchivePath(strSourceUrl, "rar", strRarPath, strPathInRar);
          else
            strSourceUrl = strPathInRar;

          CStdString strDestFile;
          strDestFile.Format("special://temp/subtitle%s%s", sub_exts[iPos],strExtExt.c_str());

          if (CFile::Cache(strSourceUrl,strDestFile))
          {
            vecExtensionsCached.push_back(CStdString(sub_exts[iPos]));
            CLog::Log(LOGINFO, " cached subtitle %s->%s", strPathInRar.c_str(), strDestFile.c_str());
            bFoundSubs = true;
            break;
          }
        }

        iPos++;
      }
  }
  return bFoundSubs;
}

int64_t CUtil::ToInt64(uint32_t high, uint32_t low)
{
  int64_t n;
  n = high;
  n <<= 32;
  n += low;
  return n;
}

bool CUtil::IsDOSPath(const CStdString &path)
{
  if (path.size() > 1 && path[1] == ':' && isalpha(path[0]))
    return true;

  return false;
}

void CUtil::AddFileToFolder(const CStdString& strFolder, const CStdString& strFile, CStdString& strResult)
{
  if(strFolder.Find("://") >= 0)
  {
    CURL url(strFolder);
    if (url.GetFileName() != strFolder)
    {
      AddFileToFolder(url.GetFileName(), strFile, strResult);
      url.SetFileName(strResult);
      strResult = url.Get();
      return;
    }
  }

  strResult = strFolder;
  if(!strResult.IsEmpty())
    AddSlashAtEnd(strResult);

  // Remove any slash at the start of the file
  if (strFile.size() && (strFile[0] == '/' || strFile[0] == '\\'))
    strResult += strFile.Mid(1);
  else
    strResult += strFile;

  // correct any slash directions
  if (!IsDOSPath(strFolder))
    strResult.Replace('\\', '/');
  else
    strResult.Replace('/', '\\');
}

void CUtil::AddSlashAtEnd(CStdString& strFolder)
{
  if(strFolder.Find("://") >= 0)
  {
    CURL url(strFolder);
    CStdString file = url.GetFileName();
    if(!file.IsEmpty() && file != strFolder)
    {
      AddSlashAtEnd(file);
      url.SetFileName(file);
    }
    strFolder = url.Get();
    return;
  }

  if (!CUtil::HasSlashAtEnd(strFolder))
  {
    if (IsDOSPath(strFolder))
      strFolder += '\\';
    else
      strFolder += '/';
  }
}

void CUtil::RemoveSlashAtEnd(CStdString& strFolder)
{
  if(strFolder.Find("://") >= 0)
  {
    CURL url(strFolder);
    CStdString file = url.GetFileName();
    if (!file.IsEmpty() && file != strFolder)
    {
      RemoveSlashAtEnd(file);
      url.SetFileName(file);
    }
    strFolder = url.Get();
    return;
  }

  while (CUtil::HasSlashAtEnd(strFolder))
    strFolder.Delete(strFolder.size() - 1);
}

void CUtil::GetDirectory(const CStdString& strFilePath, CStdString& strDirectoryPath)
{
  // Will from a full filename return the directory the file resides in.
  // Keeps the final slash at end

  int iPos1 = strFilePath.ReverseFind('/');
  int iPos2 = strFilePath.ReverseFind('\\');

  if (iPos2 > iPos1)
  {
    iPos1 = iPos2;
  }

  if (iPos1 > 0)
  {
    strDirectoryPath = strFilePath.Left(iPos1 + 1); // include the slash
  }
}

void CUtil::Split(const CStdString& strFileNameAndPath, CStdString& strPath, CStdString& strFileName)
{
  //Splits a full filename in path and file.
  //ex. smb://computer/share/directory/filename.ext -> strPath:smb://computer/share/directory/ and strFileName:filename.ext
  //Trailing slash will be preserved
  strFileName = "";
  strPath = "";
  int i = strFileNameAndPath.size() - 1;
  while (i > 0)
  {
    char ch = strFileNameAndPath[i];
    // Only break on ':' if it's a drive separator for DOS (ie d:foo)
    if (ch == '/' || ch == '\\' || (ch == ':' && i == 1)) break;
    else i--;
  }
  if (i == 0)
    i--;

  strPath = strFileNameAndPath.Left(i + 1);
  strFileName = strFileNameAndPath.Right(strFileNameAndPath.size() - i - 1);
}

void CUtil::CreateArchivePath(CStdString& strUrlPath, const CStdString& strType,
                              const CStdString& strArchivePath,
                              const CStdString& strFilePathInArchive,
                              const CStdString& strPwd)
{
  CStdString strBuffer;

  strUrlPath = strType+"://";

  if( !strPwd.IsEmpty() )
  {
    strBuffer = strPwd;
    CUtil::URLEncode(strBuffer);
    strUrlPath += strBuffer;
    strUrlPath += "@";
  }

  strBuffer = strArchivePath;
  CUtil::URLEncode(strBuffer);

  strUrlPath += strBuffer;

  strBuffer = strFilePathInArchive;
  strBuffer.Replace('\\', '/');
  strBuffer.TrimLeft('/');

  strUrlPath += "/";
  strUrlPath += strBuffer;

#if 0 // options are not used
  strBuffer = strCachePath;
  CUtil::URLEncode(strBuffer);

  strUrlPath += "?cache=";
  strUrlPath += strBuffer;

  strBuffer.Format("%i", wOptions);
  strUrlPath += "&flags=";
  strUrlPath += strBuffer;
#endif
}

bool CUtil::ThumbExists(const CStdString& strFileName, bool bAddCache)
{
  return CThumbnailCache::GetThumbnailCache()->ThumbExists(strFileName, bAddCache);
}

void CUtil::ThumbCacheAdd(const CStdString& strFileName, bool bFileExists)
{
  CThumbnailCache::GetThumbnailCache()->Add(strFileName, bFileExists);
}

void CUtil::ThumbCacheClear()
{
  CThumbnailCache::GetThumbnailCache()->Clear();
}

bool CUtil::ThumbCached(const CStdString& strFileName)
{
  return CThumbnailCache::GetThumbnailCache()->IsCached(strFileName);
}

void CUtil::PlayDVD()
{
#ifdef HAS_DVDPLAYER
  CIoSupport::Dismount("Cdrom0");
  CIoSupport::RemapDriveLetter('D', "Cdrom0");
  CFileItem item("dvd://1", false);
  item.SetLabel(g_mediaManager.GetDiskLabel());
  g_application.PlayFile(item);
#endif
}

CStdString CUtil::GetNextFilename(const CStdString &fn_template, int max)
{
  if (!fn_template.Find("%03d"))
    return "";

  for (int i = 0; i <= max; i++)
  {
    CStdString name;
    name.Format(fn_template.c_str(), i);

    WIN32_FIND_DATA wfd;
    HANDLE hFind;
    memset(&wfd, 0, sizeof(wfd));
    if ((hFind = FindFirstFile(_P(name).c_str(), &wfd)) != INVALID_HANDLE_VALUE)
      FindClose(hFind);
    else
    {
      // FindFirstFile didn't find the file 'szName', return it
      return name;
    }
  }
  return "";
}

void CUtil::Tokenize(const CStdString& path, vector<CStdString>& tokens, const string& delimiters)
{
  // Tokenize ripped from http://www.linuxselfhelp.com/HOWTO/C++Programming-HOWTO-7.html
  // Skip delimiters at beginning.
  string::size_type lastPos = path.find_first_not_of(delimiters, 0);
  // Find first "non-delimiter".
  string::size_type pos = path.find_first_of(delimiters, lastPos);

  while (string::npos != pos || string::npos != lastPos)
  {
    // Found a token, add it to the vector.
    tokens.push_back(path.substr(lastPos, pos - lastPos));
    // Skip delimiters.  Note the "not_of"
    lastPos = path.find_first_not_of(delimiters, pos);
    // Find next "non-delimiter"
    pos = path.find_first_of(delimiters, lastPos);
  }
}

void CUtil::TakeScreenshot(const CStdString &filename)
{
#ifdef HAS_DX
    LPDIRECT3DSURFACE9 lpSurface = NULL;

    g_graphicsContext.Lock();
    if (g_application.IsPlayingVideo())
    {
#ifdef HAS_VIDEO_PLAYBACK
      g_renderManager.SetupScreenshot();
#endif
    }
    if (0)
    { // reset calibration to defaults
      OVERSCAN oscan;
      memcpy(&oscan, &g_settings.m_ResInfo[g_graphicsContext.GetVideoResolution()].Overscan, sizeof(OVERSCAN));
      g_graphicsContext.ResetOverscan(g_graphicsContext.GetVideoResolution(), g_settings.m_ResInfo[g_graphicsContext.GetVideoResolution()].Overscan);
      g_application.Render();
      memcpy(&g_settings.m_ResInfo[g_graphicsContext.GetVideoResolution()].Overscan, &oscan, sizeof(OVERSCAN));
    }
    // now take screenshot
    g_application.RenderNoPresent();
    if (SUCCEEDED(g_Windowing.Get3DDevice()->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &lpSurface)))
    {
      if (FAILED(XGWriteSurfaceToFile(lpSurface, filename.c_str())))
      {
        CLog::Log(LOGERROR, "Failed to Generate Screenshot");
      }
      else
      {
        CLog::Log(LOGINFO, "Screen shot saved as %s", filename.c_str());
      }
      lpSurface->Release();
    }
    g_graphicsContext.Unlock();
#else

#endif

#if defined(HAS_GL)

    g_graphicsContext.BeginPaint();
    if (g_application.IsPlayingVideo())
    {
#ifdef HAS_VIDEO_PLAYBACK
      g_renderManager.SetupScreenshot();
#endif
    }
    g_application.RenderNoPresent();

    GLint viewport[4];
    void *pixels = NULL;
    glReadBuffer(GL_BACK);
    glGetIntegerv(GL_VIEWPORT, viewport);
    pixels = malloc(viewport[2] * viewport[3] * 4);
    if (pixels)
    {
      glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_BGRA, GL_UNSIGNED_BYTE, pixels);
      XGWriteSurfaceToFile(pixels, viewport[2], viewport[3], filename.c_str());
      free(pixels);
    }
    g_graphicsContext.EndPaint();

#endif

}

void CUtil::TakeScreenshot()
{
  static bool savingScreenshots = false;
  static vector<CStdString> screenShots;

  bool promptUser = false;
  // check to see if we have a screenshot folder yet
  CStdString strDir = g_guiSettings.GetString("system.screenshotpath", false);
  if (strDir.IsEmpty())
  {
    strDir = "special://temp/";
    if (!savingScreenshots)
    {
      promptUser = true;
      savingScreenshots = true;
      screenShots.clear();
    }
  }
  CUtil::RemoveSlashAtEnd(strDir);

  if (!strDir.IsEmpty())
  {
    CStdString file = CUtil::GetNextFilename(CUtil::AddFileToFolder(strDir, "screenshot%03d.bmp"), 999);

    if (!file.IsEmpty())
    {
      TakeScreenshot(file);
      if (savingScreenshots)
        screenShots.push_back(file);
      if (promptUser)
      { // grab the real directory
        CStdString newDir = g_guiSettings.GetString("system.screenshotpath");
        if (!newDir.IsEmpty())
        {
          for (unsigned int i = 0; i < screenShots.size(); i++)
          {
            CStdString file = CUtil::GetNextFilename(CUtil::AddFileToFolder(newDir, "screenshot%03d.bmp"), 999);
            CFile::Cache(screenShots[i], file);
          }
          screenShots.clear();
        }
        savingScreenshots = false;
      }
    }
    else
    {
      CLog::Log(LOGWARNING, "Too many screen shots or invalid folder");
    }
  }
}

void CUtil::ClearCache()
{
  for (int i = 0; i < 16; i++)
  {
    CStdString strHex, folder;
    strHex.Format("%x", i);
    CUtil::AddFileToFolder(g_settings.GetMusicThumbFolder(), strHex, folder);
    g_directoryCache.ClearDirectory(folder);
  }
}

void CUtil::StatToStatI64(struct _stati64 *result, struct stat *stat)
{
  result->st_dev = stat->st_dev;
  result->st_ino = stat->st_ino;
  result->st_mode = stat->st_mode;
  result->st_nlink = stat->st_nlink;
  result->st_uid = stat->st_uid;
  result->st_gid = stat->st_gid;
  result->st_rdev = stat->st_rdev;
  result->st_size = (int64_t)stat->st_size;

#ifndef _LINUX
  result->st_atime = (long)(stat->st_atime & 0xFFFFFFFF);
  result->st_mtime = (long)(stat->st_mtime & 0xFFFFFFFF);
  result->st_ctime = (long)(stat->st_ctime & 0xFFFFFFFF);
#else
  result->_st_atime = (long)(stat->st_atime & 0xFFFFFFFF);
  result->_st_mtime = (long)(stat->st_mtime & 0xFFFFFFFF);
  result->_st_ctime = (long)(stat->st_ctime & 0xFFFFFFFF);
#endif
}

void CUtil::Stat64ToStatI64(struct _stati64 *result, struct __stat64 *stat)
{
  result->st_dev = stat->st_dev;
  result->st_ino = stat->st_ino;
  result->st_mode = stat->st_mode;
  result->st_nlink = stat->st_nlink;
  result->st_uid = stat->st_uid;
  result->st_gid = stat->st_gid;
  result->st_rdev = stat->st_rdev;
  result->st_size = stat->st_size;
#ifndef _LINUX
  result->st_atime = (long)(stat->st_atime & 0xFFFFFFFF);
  result->st_mtime = (long)(stat->st_mtime & 0xFFFFFFFF);
  result->st_ctime = (long)(stat->st_ctime & 0xFFFFFFFF);
#else
  result->_st_atime = (long)(stat->st_atime & 0xFFFFFFFF);
  result->_st_mtime = (long)(stat->st_mtime & 0xFFFFFFFF);
  result->_st_ctime = (long)(stat->st_ctime & 0xFFFFFFFF);
#endif
}

void CUtil::StatI64ToStat64(struct __stat64 *result, struct _stati64 *stat)
{
  result->st_dev = stat->st_dev;
  result->st_ino = stat->st_ino;
  result->st_mode = stat->st_mode;
  result->st_nlink = stat->st_nlink;
  result->st_uid = stat->st_uid;
  result->st_gid = stat->st_gid;
  result->st_rdev = stat->st_rdev;
  result->st_size = stat->st_size;
#ifndef _LINUX
  result->st_atime = stat->st_atime;
  result->st_mtime = stat->st_mtime;
  result->st_ctime = stat->st_ctime;
#else
  result->st_atime = stat->_st_atime;
  result->st_mtime = stat->_st_mtime;
  result->st_ctime = stat->_st_ctime;
#endif
}

void CUtil::Stat64ToStat(struct stat *result, struct __stat64 *stat)
{
  result->st_dev = stat->st_dev;
  result->st_ino = stat->st_ino;
  result->st_mode = stat->st_mode;
  result->st_nlink = stat->st_nlink;
  result->st_uid = stat->st_uid;
  result->st_gid = stat->st_gid;
  result->st_rdev = stat->st_rdev;
#ifndef _LINUX
  if (stat->st_size <= LONG_MAX)
    result->st_size = (_off_t)stat->st_size;
#else
  if (sizeof(stat->st_size) <= sizeof(result->st_size) )
    result->st_size = (off_t)stat->st_size;
#endif
  else
  {
    result->st_size = 0;
    CLog::Log(LOGWARNING, "WARNING: File is larger than 32bit stat can handle, file size will be reported as 0 bytes");
  }
  result->st_atime = (time_t)stat->st_atime;
  result->st_mtime = (time_t)stat->st_mtime;
  result->st_ctime = (time_t)stat->st_ctime;
}

bool CUtil::CreateDirectoryEx(const CStdString& strPath)
{
  // Function to create all directories at once instead
  // of calling CreateDirectory for every subdir.
  // Creates the directory and subdirectories if needed.

  // return true if directory already exist
  if (CDirectory::Exists(strPath)) return true;
  
  // we currently only allow HD and smb paths
  if (!CUtil::IsHD(strPath) && !CUtil::IsSmb(strPath))
  {
    CLog::Log(LOGERROR,"%s called with an unsupported path: %s", __FUNCTION__, strPath.c_str());
    return false;
  }
  
  CURL url(strPath);
  // silly CStdString can't take a char in the constructor
  CStdString sep(1, url.GetDirectorySeparator());
  
  // split the filename portion of the URL up into separate dirs
  CStdStringArray dirs;
  StringUtils::SplitString(url.GetFileName(), sep, dirs);
  
  // we start with the root path
  CStdString dir = url.GetWithoutFilename();
  unsigned int i = 0;
  if (dir.IsEmpty())
  { // local directory - start with the first dirs member so that
    // we ensure CUtil::AddFileToFolder() below has something to work with
    dir = dirs[i++] + sep;
  }
  // and append the rest of the directories successively, creating each dir
  // as we go
  for (; i < dirs.size(); i++)
  {
    dir = CUtil::AddFileToFolder(dir, dirs[i]);
    CDirectory::Create(dir);
  }
  
  // was the final destination directory successfully created ?
  if (!CDirectory::Exists(strPath)) return false;
  return true;
}

CStdString CUtil::MakeLegalFileName(const CStdString &strFile, int LegalType)
{
  CStdString result = strFile;

  result.Replace('/', '_');
  result.Replace('\\', '_');
  result.Replace('?', '_');

  if (LegalType == LEGAL_WIN32_COMPAT)
  {
    // just filter out some illegal characters on windows
    result.Replace(':', '_');
    result.Replace('*', '_');
    result.Replace('?', '_');
    result.Replace('\"', '_');
    result.Replace('<', '_');
    result.Replace('>', '_');
    result.Replace('|', '_');
    result.TrimRight(".");
    result.TrimRight(" ");
  }
  return result;
}

// same as MakeLegalFileName, but we assume that we're passed a complete path,
// and just legalize the filename
CStdString CUtil::MakeLegalPath(const CStdString &strPathAndFile, int LegalType)
{
  CStdString strPath;
  GetDirectory(strPathAndFile,strPath);
  CStdString strFileName = GetFileName(strPathAndFile);
  return strPath + MakeLegalFileName(strFileName, LegalType);
}

bool CUtil::IsUsingTTFSubtitles()
{
  return CUtil::GetExtension(g_guiSettings.GetString("subtitles.font")).Equals(".ttf");
}

void CUtil::SplitExecFunction(const CStdString &execString, CStdString &function, vector<CStdString> &parameters)
{
  CStdString paramString;
 
  int iPos = execString.Find("(");
  int iPos2 = execString.ReverseFind(")");
  if (iPos > 0 && iPos2 > 0)
  {
    paramString = execString.Mid(iPos + 1, iPos2 - iPos - 1);
    function = execString.Left(iPos);
  }
  else
    function = execString;

  // remove any whitespace, and the standard prefix (if it exists)
  function.Trim();
  if( function.Left(5).Equals("xbmc.", false) )
    function.Delete(0, 5);

  // now split up our parameters - we may have quotes to deal with as well as brackets and whitespace
  bool inQuotes = false;
  int inFunction = 0;
  size_t whiteSpacePos = 0;
  CStdString parameter;
  parameters.clear();
  for (size_t pos = 0; pos < paramString.size(); pos++)
  {
    char ch = paramString[pos];
    bool escaped = (pos > 0 && paramString[pos - 1] == '\\');
    if (inQuotes)
    { // if we're in a quote, we accept everything until the closing quote
      if (ch == '\"' && !escaped)
      { // finished a quote - no need to add the end quote to our string
        inQuotes = false;
        continue;
      }
    }
    else
    { // not in a quote, so check if we should be starting one
      if (ch == '\"' && !escaped)
      { // start of quote - no need to add the quote to our string
        inQuotes = true;
        continue;
      }
      if (inFunction && ch == ')')
      { // end of a function
        inFunction--;
      }
      if (ch == '(')
      { // start of function
        inFunction++;
      }
      if (!inFunction && !IsStack(paramString) && ch == ',')
      { // not in a function, so a comma signfies the end of this parameter
        if (whiteSpacePos)
          parameter = parameter.Left(whiteSpacePos);
        parameters.push_back(parameter);
        parameter.Empty();
        whiteSpacePos = 0;
        continue;
      }
    }
    if (ch == '\"' && escaped)
    { // escape quote
      parameter[parameter.size()-1] = ch;
      continue;
    }
    // whitespace handling - we skip any whitespace at the left or right of an unquoted parameter
    if (ch == ' ' && !inQuotes)
    {
      if (parameter.IsEmpty()) // skip whitespace on left
        continue;
      if (!whiteSpacePos) // make a note of where whitespace starts on the right
        whiteSpacePos = parameter.size();
    }
    else
      whiteSpacePos = 0;
    parameter += ch;
  }
  if (inFunction || inQuotes)
    CLog::Log(LOGWARNING, "%s(%s) - end of string while searching for ) or \"", __FUNCTION__, execString.c_str());
  if (whiteSpacePos)
    parameter = parameter.Left(whiteSpacePos);
  if (!parameter.IsEmpty())
    parameters.push_back(parameter);
}

int CUtil::GetMatchingSource(const CStdString& strPath1, VECSOURCES& VECSOURCES, bool& bIsSourceName)
{
  if (strPath1.IsEmpty())
    return -1;

  //CLog::Log(LOGDEBUG,"CUtil::GetMatchingSource, testing original path/name [%s]", strPath1.c_str());

  // copy as we may change strPath
  CStdString strPath = strPath1;

  // Check for special protocols
  CURL checkURL(strPath);

  // stack://
  if (checkURL.GetProtocol() == "stack")
    strPath.Delete(0, 8); // remove the stack protocol

  if (checkURL.GetProtocol() == "shout")
    strPath = checkURL.GetHostName();
  if (checkURL.GetProtocol() == "lastfm")
    return 1;
  if (checkURL.GetProtocol() == "tuxbox")
    return 1;
  if (checkURL.GetProtocol() == "plugin")
    return 1;
  if (checkURL.GetProtocol() == "multipath")
    strPath = CMultiPathDirectory::GetFirstPath(strPath);

  //CLog::Log(LOGDEBUG,"CUtil::GetMatchingSource, testing for matching name [%s]", strPath.c_str());
  bIsSourceName = false;
  int iIndex = -1;
  int iLength = -1;
  // we first test the NAME of a source
  for (int i = 0; i < (int)VECSOURCES.size(); ++i)
  {
    CMediaSource share = VECSOURCES.at(i);
    CStdString strName = share.strName;

    // special cases for dvds
    if (IsOnDVD(share.strPath))
    {
      if (IsOnDVD(strPath))
        return i;

      // not a path, so we need to modify the source name
      // since we add the drive status and disc name to the source
      // "Name (Drive Status/Disc Name)"
      int iPos = strName.ReverseFind('(');
      if (iPos > 1)
        strName = strName.Mid(0, iPos - 1);
    }
    //CLog::Log(LOGDEBUG,"CUtil::GetMatchingSource, comparing name [%s]", strName.c_str());
    if (strPath.Equals(strName))
    {
      bIsSourceName = true;
      return i;
    }
  }

  // now test the paths

  // remove user details, and ensure path only uses forward slashes
  // and ends with a trailing slash so as not to match a substring
  CURL urlDest(strPath);
  urlDest.SetOptions("");
  CStdString strDest = urlDest.GetWithoutUserDetails();
  ForceForwardSlashes(strDest);
  if (!HasSlashAtEnd(strDest))
    strDest += "/";
  int iLenPath = strDest.size();

  //CLog::Log(LOGDEBUG,"CUtil::GetMatchingSource, testing url [%s]", strDest.c_str());

  for (int i = 0; i < (int)VECSOURCES.size(); ++i)
  {
    CMediaSource share = VECSOURCES.at(i);

    // does it match a source name?
    if (share.strPath.substr(0,8) == "shout://")
    {
      CURL url(share.strPath);
      if (strPath.Equals(url.GetHostName()))
        return i;
    }

    // doesnt match a name, so try the source path
    vector<CStdString> vecPaths;

    // add any concatenated paths if they exist
    if (share.vecPaths.size() > 0)
      vecPaths = share.vecPaths;

    // add the actual share path at the front of the vector
    vecPaths.insert(vecPaths.begin(), share.strPath);

    // test each path
    for (int j = 0; j < (int)vecPaths.size(); ++j)
    {
      // remove user details, and ensure path only uses forward slashes
      // and ends with a trailing slash so as not to match a substring
      CURL urlShare(vecPaths[j]);
      urlShare.SetOptions("");
      CStdString strShare = urlShare.GetWithoutUserDetails();
      ForceForwardSlashes(strShare);
      if (!HasSlashAtEnd(strShare))
        strShare += "/";
      int iLenShare = strShare.size();
      //CLog::Log(LOGDEBUG,"CUtil::GetMatchingSource, comparing url [%s]", strShare.c_str());

      if ((iLenPath >= iLenShare) && (strDest.Left(iLenShare).Equals(strShare)) && (iLenShare > iLength))
      {
        //CLog::Log(LOGDEBUG,"Found matching source at index %i: [%s], Len = [%i]", i, strShare.c_str(), iLenShare);

        // if exact match, return it immediately
        if (iLenPath == iLenShare)
        {
          // if the path EXACTLY matches an item in a concatentated path
          // set source name to true to load the full virtualpath
          bIsSourceName = false;
          if (vecPaths.size() > 1)
            bIsSourceName = true;
          return i;
        }
        iIndex = i;
        iLength = iLenShare;
      }
    }
  }

  // return the index of the share with the longest match
  if (iIndex == -1)
  {

    // rar:// and zip://
    // if archive wasn't mounted, look for a matching share for the archive instead
    if( strPath.Left(6).Equals("rar://") || strPath.Left(6).Equals("zip://") )
    {
      // get the hostname portion of the url since it contains the archive file
      strPath = checkURL.GetHostName();

      bIsSourceName = false;
      bool bDummy;
      return GetMatchingSource(strPath, VECSOURCES, bDummy);
    }

    CLog::Log(LOGWARNING,"CUtil::GetMatchingSource... no matching source found for [%s]", strPath1.c_str());
  }
  return iIndex;
}

CStdString CUtil::TranslateSpecialSource(const CStdString &strSpecial)
{
  CStdString strReturn=strSpecial;
  if (!strSpecial.IsEmpty() && strSpecial[0] == '$')
  {
    if (strSpecial.Left(5).Equals("$HOME"))
      CUtil::AddFileToFolder("special://home/", strSpecial.Mid(5), strReturn);
    else if (strSpecial.Left(10).Equals("$SUBTITLES"))
      CUtil::AddFileToFolder("special://subtitles/", strSpecial.Mid(10), strReturn);
    else if (strSpecial.Left(9).Equals("$USERDATA"))
      CUtil::AddFileToFolder("special://userdata/", strSpecial.Mid(9), strReturn);
    else if (strSpecial.Left(9).Equals("$DATABASE"))
      CUtil::AddFileToFolder("special://database/", strSpecial.Mid(9), strReturn);
    else if (strSpecial.Left(11).Equals("$THUMBNAILS"))
      CUtil::AddFileToFolder("special://thumbnails/", strSpecial.Mid(11), strReturn);
    else if (strSpecial.Left(11).Equals("$RECORDINGS"))
      CUtil::AddFileToFolder("special://recordings/", strSpecial.Mid(11), strReturn);
    else if (strSpecial.Left(12).Equals("$SCREENSHOTS"))
      CUtil::AddFileToFolder("special://screenshots/", strSpecial.Mid(12), strReturn);
    else if (strSpecial.Left(15).Equals("$MUSICPLAYLISTS"))
      CUtil::AddFileToFolder("special://musicplaylists/", strSpecial.Mid(15), strReturn);
    else if (strSpecial.Left(15).Equals("$VIDEOPLAYLISTS"))
      CUtil::AddFileToFolder("special://videoplaylists/", strSpecial.Mid(15), strReturn);
    else if (strSpecial.Left(7).Equals("$CDRIPS"))
      CUtil::AddFileToFolder("special://cdrips/", strSpecial.Mid(7), strReturn);
    // this one will be removed post 2.0
    else if (strSpecial.Left(10).Equals("$PLAYLISTS"))
      CUtil::AddFileToFolder(g_guiSettings.GetString("system.playlistspath",false), strSpecial.Mid(10), strReturn);
  }
  return strReturn;
}

CStdString CUtil::MusicPlaylistsLocation()
{
  vector<CStdString> vec;
  CStdString strReturn;
  CUtil::AddFileToFolder(g_guiSettings.GetString("system.playlistspath"), "music", strReturn);
  vec.push_back(strReturn);
  CUtil::AddFileToFolder(g_guiSettings.GetString("system.playlistspath"), "mixed", strReturn);
  vec.push_back(strReturn);
  return DIRECTORY::CMultiPathDirectory::ConstructMultiPath(vec);;
}

CStdString CUtil::VideoPlaylistsLocation()
{
  vector<CStdString> vec;
  CStdString strReturn;
  CUtil::AddFileToFolder(g_guiSettings.GetString("system.playlistspath"), "video", strReturn);
  vec.push_back(strReturn);
  CUtil::AddFileToFolder(g_guiSettings.GetString("system.playlistspath"), "mixed", strReturn);
  vec.push_back(strReturn);
  return DIRECTORY::CMultiPathDirectory::ConstructMultiPath(vec);;
}

void CUtil::DeleteMusicDatabaseDirectoryCache()
{
  CUtil::DeleteDirectoryCache("mdb");
}

void CUtil::DeleteVideoDatabaseDirectoryCache()
{
  CUtil::DeleteDirectoryCache("vdb");
}

void CUtil::DeleteDirectoryCache(const CStdString strType /* = ""*/)
{
  WIN32_FIND_DATA wfd;
  memset(&wfd, 0, sizeof(wfd));

  CStdString strFile = "special://temp/";
  if (!strType.IsEmpty())
  {
    strFile += strType;
    if (!strFile.Right(1).Equals("-"))
      strFile += "-";
  }
  strFile += "*.fi";
  CAutoPtrFind hFind(FindFirstFile(_P(strFile).c_str(), &wfd));
  if (!hFind.isValid())
    return;
  do
  {
    if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      CFile::Delete(CUtil::AddFileToFolder("special://temp/", wfd.cFileName));
  }
  while (FindNextFile(hFind, &wfd));
}

bool CUtil::SetSysDateTimeYear(int iYear, int iMonth, int iDay, int iHour, int iMinute)
{
  TIME_ZONE_INFORMATION tziNew;
  SYSTEMTIME CurTime;
  SYSTEMTIME NewTime;
  GetLocalTime(&CurTime);
  GetLocalTime(&NewTime);
  int iRescBiases, iHourUTC;
  int iMinuteNew;

  DWORD dwRet = GetTimeZoneInformation(&tziNew);  // Get TimeZone Informations
  float iGMTZone = (float(tziNew.Bias)/(60));     // Calc's the GMT Time

  CLog::Log(LOGDEBUG, "------------ TimeZone -------------");
  CLog::Log(LOGDEBUG, "-      GMT Zone: GMT %.1f",iGMTZone);
  CLog::Log(LOGDEBUG, "-          Bias: %lu minutes",tziNew.Bias);
  CLog::Log(LOGDEBUG, "-  DaylightBias: %lu",tziNew.DaylightBias);
  CLog::Log(LOGDEBUG, "-  StandardBias: %lu",tziNew.StandardBias);

  switch (dwRet)
  {
    case TIME_ZONE_ID_STANDARD:
      {
        iRescBiases   = tziNew.Bias + tziNew.StandardBias;
        CLog::Log(LOGDEBUG, "-   Timezone ID: 1, Standart");
      }
      break;
    case TIME_ZONE_ID_DAYLIGHT:
      {
        iRescBiases   = tziNew.Bias + tziNew.StandardBias + tziNew.DaylightBias;
        CLog::Log(LOGDEBUG, "-   Timezone ID: 2, Daylight");
      }
      break;
    case TIME_ZONE_ID_UNKNOWN:
      {
        iRescBiases   = tziNew.Bias + tziNew.StandardBias;
        CLog::Log(LOGDEBUG, "-   Timezone ID: 0, Unknown");
      }
      break;
    case TIME_ZONE_ID_INVALID:
      {
        iRescBiases   = tziNew.Bias + tziNew.StandardBias;
        CLog::Log(LOGDEBUG, "-   Timezone ID: Invalid");
      }
      break;
    default:
      iRescBiases   = tziNew.Bias + tziNew.StandardBias;
  }
    CLog::Log(LOGDEBUG, "--------------- END ---------------");

  // Calculation
  iHourUTC = GMTZoneCalc(iRescBiases, iHour, iMinute, iMinuteNew);
  iMinute = iMinuteNew;
  if(iHourUTC <0)
  {
    iDay = iDay - 1;
    iHourUTC =iHourUTC + 24;
  }
  if(iHourUTC >23)
  {
    iDay = iDay + 1;
    iHourUTC =iHourUTC - 24;
  }

  // Set the New-,Detected Time Values to System Time!
  NewTime.wYear     = (WORD)iYear;
  NewTime.wMonth    = (WORD)iMonth;
  NewTime.wDay      = (WORD)iDay;
  NewTime.wHour     = (WORD)iHourUTC;
  NewTime.wMinute   = (WORD)iMinute;

  FILETIME stNewTime, stCurTime;
  SystemTimeToFileTime(&NewTime, &stNewTime);
  SystemTimeToFileTime(&CurTime, &stCurTime);
  return false;
}
int CUtil::GMTZoneCalc(int iRescBiases, int iHour, int iMinute, int &iMinuteNew)
{
  int iHourUTC, iTemp;
  iMinuteNew = iMinute;
  iTemp = iRescBiases/60;

  if (iRescBiases == 0 )return iHour;   // GMT Zone 0, no need calculate
  if (iRescBiases > 0)
    iHourUTC = iHour + abs(iTemp);
  else
    iHourUTC = iHour - abs(iTemp);

  if ((iTemp*60) != iRescBiases)
  {
    if (iRescBiases > 0)
      iMinuteNew = iMinute + abs(iTemp*60 - iRescBiases);
    else
      iMinuteNew = iMinute - abs(iTemp*60 - iRescBiases);

    if (iMinuteNew >= 60)
    {
      iMinuteNew = iMinuteNew -60;
      iHourUTC = iHourUTC + 1;
    }
    else if (iMinuteNew < 0)
    {
      iMinuteNew = iMinuteNew +60;
      iHourUTC = iHourUTC - 1;
    }
  }
  return iHourUTC;
}

void CUtil::GetRecursiveListing(const CStdString& strPath, CFileItemList& items, const CStdString& strMask, bool bUseFileDirectories)
{
  CFileItemList myItems;
  CDirectory::GetDirectory(strPath,myItems,strMask,bUseFileDirectories);
  for (int i=0;i<myItems.Size();++i)
  {
    if (myItems[i]->m_bIsFolder)
      CUtil::GetRecursiveListing(myItems[i]->m_strPath,items,strMask,bUseFileDirectories);
    else if (!myItems[i]->IsRAR() && !myItems[i]->IsZIP())
      items.Add(myItems[i]);
  }
}

void CUtil::GetRecursiveDirsListing(const CStdString& strPath, CFileItemList& item)
{
  CFileItemList myItems;
  CDirectory::GetDirectory(strPath,myItems,"",false);
  for (int i=0;i<myItems.Size();++i)
  {
    if (myItems[i]->m_bIsFolder && !myItems[i]->m_strPath.Equals(".."))
    {
      item.Add(myItems[i]);
      CUtil::GetRecursiveDirsListing(myItems[i]->m_strPath,item);
    }
  }
}

void CUtil::ForceForwardSlashes(CStdString& strPath)
{
  int iPos = strPath.ReverseFind('\\');
  while (iPos > 0)
  {
    strPath.at(iPos) = '/';
    iPos = strPath.ReverseFind('\\');
  }
}

double CUtil::AlbumRelevance(const CStdString& strAlbumTemp1, const CStdString& strAlbum1, const CStdString& strArtistTemp1, const CStdString& strArtist1)
{
  // case-insensitive fuzzy string comparison on the album and artist for relevance
  // weighting is identical, both album and artist are 50% of the total relevance
  // a missing artist means the maximum relevance can only be 0.50
  CStdString strAlbumTemp = strAlbumTemp1;
  strAlbumTemp.MakeLower();
  CStdString strAlbum = strAlbum1;
  strAlbum.MakeLower();
  double fAlbumPercentage = fstrcmp(strAlbumTemp, strAlbum, 0.0f);
  double fArtistPercentage = 0.0f;
  if (!strArtist1.IsEmpty())
  {
    CStdString strArtistTemp = strArtistTemp1;
    strArtistTemp.MakeLower();
    CStdString strArtist = strArtist1;
    strArtist.MakeLower();
    fArtistPercentage = fstrcmp(strArtistTemp, strArtist, 0.0f);
  }
  double fRelevance = fAlbumPercentage * 0.5f + fArtistPercentage * 0.5f;
  return fRelevance;
}

CStdString CUtil::SubstitutePath(const CStdString& strFileName)
{
  //CLog::Log(LOGDEBUG,"%s checking source filename:[%s]", __FUNCTION__, strFileName.c_str());
  // substitutes paths to correct issues with remote playlists containing full paths
  for (unsigned int i = 0; i < g_advancedSettings.m_pathSubstitutions.size(); i++)
  {
    vector<CStdString> vecSplit;
    StringUtils::SplitString(g_advancedSettings.m_pathSubstitutions[i], " , ", vecSplit);

    // something is wrong, go to next substitution
    if (vecSplit.size() != 2)
      continue;

    CStdString strSearch = vecSplit[0];
    CStdString strReplace = vecSplit[1];
    strSearch.Replace(",,",",");
    strReplace.Replace(",,",",");

    CUtil::AddSlashAtEnd(strSearch);
    CUtil::AddSlashAtEnd(strReplace);

    // if left most characters match the search, replace them
    //CLog::Log(LOGDEBUG,"%s testing for path:[%s]", __FUNCTION__, strSearch.c_str());
    int iLen = strSearch.size();
    if (strFileName.Left(iLen).Equals(strSearch))
    {
      // fix slashes
      CStdString strTemp = strFileName.Mid(iLen);
      strTemp.Replace("\\", strReplace.Right(1));
      CStdString strFileNameNew = strReplace + strTemp;
      //CLog::Log(LOGDEBUG,"%s new filename:[%s]", __FUNCTION__, strFileNameNew.c_str());
      return strFileNameNew;
    }
  }
  // nothing matches, return original string
  //CLog::Log(LOGDEBUG,"%s no matches", __FUNCTION__);
  return strFileName;
}

bool CUtil::MakeShortenPath(CStdString StrInput, CStdString& StrOutput, int iTextMaxLength)
{
  int iStrInputSize = StrInput.size();
  if((iStrInputSize <= 0) || (iTextMaxLength >= iStrInputSize))
    return false;

  char cDelim = '\0';
  size_t nGreaterDelim, nPos;

  nPos = StrInput.find_last_of( '\\' );
  if ( nPos != CStdString::npos )
    cDelim = '\\';
  else
  {
    nPos = StrInput.find_last_of( '/' );
    if ( nPos != CStdString::npos )
      cDelim = '/';
  }
  if ( cDelim == '\0' )
    return false;

  if (nPos == StrInput.size() - 1)
  {
    StrInput.erase(StrInput.size() - 1);
    nPos = StrInput.find_last_of( cDelim );
  }
  while( iTextMaxLength < iStrInputSize )
  {
    nPos = StrInput.find_last_of( cDelim, nPos );
    nGreaterDelim = nPos;
    if ( nPos != CStdString::npos )
      nPos = StrInput.find_last_of( cDelim, nPos - 1 );
    if ( nPos == CStdString::npos ) break;
    if ( nGreaterDelim > nPos ) StrInput.replace( nPos + 1, nGreaterDelim - nPos - 1, ".." );
    iStrInputSize = StrInput.size();
  }
  // replace any additional /../../ with just /../ if necessary
  CStdString replaceDots;
  replaceDots.Format("..%c..", cDelim);
  while (StrInput.size() > (unsigned int)iTextMaxLength)
    if (!StrInput.Replace(replaceDots, ".."))
      break;
  // finally, truncate our string to force inside our max text length,
  // replacing the last 2 characters with ".."

  // eg end up with:
  // "smb://../Playboy Swimsuit Cal.."
  if (iTextMaxLength > 2 && StrInput.size() > (unsigned int)iTextMaxLength)
  {
    StrInput = StrInput.Left(iTextMaxLength - 2);
    StrInput += "..";
  }
  StrOutput = StrInput;
  return true;
}

bool CUtil::SupportsFileOperations(const CStdString& strPath)
{
  // currently only hd and smb support delete and rename
  if (IsHD(strPath))
    return true;
  if (IsSmb(strPath))
    return true;
  if (IsMythTV(strPath))
  {
    /*
     * Can't use CFile::Exists() to check whether the myth:// path supports file operations because
     * it hits the directory cache on the way through, which has the Live Channels and Guide
     * items cached.
     */
    return CCMythDirectory::SupportsFileOperations(strPath);
  }
  if (IsStack(strPath))
  {
    CStackDirectory dir;
    return SupportsFileOperations(dir.GetFirstStackedFile(strPath));
  }
  if (IsMultiPath(strPath))
    return CMultiPathDirectory::SupportsFileOperations(strPath);

  return false;
}

CStdString CUtil::GetCachedAlbumThumb(const CStdString& album, const CStdString& artist)
{
  if (album.IsEmpty())
    return GetCachedMusicThumb("unknown"+artist);
  if (artist.IsEmpty())
    return GetCachedMusicThumb(album+"unknown");
  return GetCachedMusicThumb(album+artist);
}

CStdString CUtil::GetCachedMusicThumb(const CStdString& path)
{
  Crc32 crc;
  CStdString noSlashPath(path);
  RemoveSlashAtEnd(noSlashPath);
  crc.ComputeFromLowerCase(noSlashPath);
  CStdString hex;
  hex.Format("%08x", (unsigned __int32) crc);
  CStdString thumb;
  thumb.Format("%c/%s.tbn", hex[0], hex.c_str());
  return CUtil::AddFileToFolder(g_settings.GetMusicThumbFolder(), thumb);
}

CStdString CUtil::GetDefaultFolderThumb(const CStdString &folderThumb)
{
  if (g_TextureManager.HasTexture(folderThumb))
    return folderThumb;
  return "";
}

void CUtil::GetSkinThemes(vector<CStdString>& vecTheme)
{
  CStdString strPath;
  CUtil::AddFileToFolder(g_graphicsContext.GetMediaDir(),"media",strPath);
  CFileItemList items;
  CDirectory::GetDirectory(strPath, items);
  // Search for Themes in the Current skin!
  for (int i = 0; i < items.Size(); ++i)
  {
    CFileItemPtr pItem = items[i];
    if (!pItem->m_bIsFolder)
    {
      CStdString strExtension;
      CUtil::GetExtension(pItem->m_strPath, strExtension);
      if (strExtension == ".xpr" && pItem->GetLabel().CompareNoCase("Textures.xpr"))
      {
        CStdString strLabel = pItem->GetLabel();
        vecTheme.push_back(strLabel.Mid(0, strLabel.size() - 4));
      }
    }
  }
  sort(vecTheme.begin(), vecTheme.end(), sortstringbyname());
}

void CUtil::WipeDir(const CStdString& strPath) // DANGEROUS!!!!
{
  if (!CDirectory::Exists(strPath)) return;

  CFileItemList items;
  CUtil::GetRecursiveListing(strPath,items,"");
  for (int i=0;i<items.Size();++i)
  {
    if (!items[i]->m_bIsFolder)
      CFile::Delete(items[i]->m_strPath);
  }
  items.Clear();
  CUtil::GetRecursiveDirsListing(strPath,items);
  for (int i=items.Size()-1;i>-1;--i) // need to wipe them backwards
  {
    CUtil::AddSlashAtEnd(items[i]->m_strPath);
    CDirectory::Remove(items[i]->m_strPath);
  }

  CStdString tmpPath = strPath;
  AddSlashAtEnd(tmpPath);
  CDirectory::Remove(tmpPath);
}

void CUtil::CopyDirRecursive(const CStdString& strSrcPath, const CStdString& strDstPath)
{
  if (!CDirectory::Exists(strSrcPath)) return;

  // create root first
  CStdString destPath;

  destPath = strDstPath;
  AddSlashAtEnd(destPath);
  CDirectory::Create(destPath);

  CFileItemList items;
  CUtil::GetRecursiveDirsListing(strSrcPath,items);
  for (int i=0;i<items.Size();++i)
  {
    destPath = items[i]->m_strPath;
    destPath.Replace(strSrcPath,"");
    destPath = CUtil::AddFileToFolder(strDstPath, destPath);
    CDirectory::Create(destPath);
  }
  items.Clear();
  CUtil::GetRecursiveListing(strSrcPath,items,"");
  for (int i=0;i<items.Size();i++)
  {
    destPath = items[i]->m_strPath;
    destPath.Replace(strSrcPath,"");
    destPath = CUtil::AddFileToFolder(strDstPath, destPath);
    CFile::Cache(items[i]->m_strPath, destPath);
  }
}

void CUtil::ClearFileItemCache()
{
  CFileItemList items;
  CDirectory::GetDirectory("special://temp/", items, ".fi", false);
  for (int i = 0; i < items.Size(); ++i)
  {
    if (!items[i]->m_bIsFolder)
      CFile::Delete(items[i]->m_strPath);
  }
}

void CUtil::InitRandomSeed()
{
  // Init random seed 
  int64_t now; 
  now = CurrentHostCounter(); 
  unsigned int seed = (unsigned int)now;
//  CLog::Log(LOGDEBUG, "%s - Initializing random seed with %u", __FUNCTION__, seed);
  srand(seed);
}

#ifdef _LINUX
bool CUtil::RunCommandLine(const CStdString& cmdLine, bool waitExit)
{
  CStdStringArray args;

  StringUtils::SplitString(cmdLine, ",", args);

  // Strip quotes and whitespace around the arguments, or exec will fail.
  // This allows the python invocation to be written more naturally with any amount of whitespace around the args.
  // But it's still limited, for example quotes inside the strings are not expanded, etc.
  // TODO: Maybe some python library routine can parse this more properly ?
  for (size_t i=0; i<args.size(); i++)
  {
    CStdString &s = args[i];
    CStdString stripd = s.Trim();
    if (stripd[0] == '"' || stripd[0] == '\'')
    {
      s = s.TrimLeft();
      s = s.Right(s.size() - 1);
    }
    if (stripd[stripd.size() - 1] == '"' || stripd[stripd.size() - 1] == '\'')
    {
      s = s.TrimRight();
      s = s.Left(s.size() - 1);
    }
  }

  return Command(args, waitExit);
}

//
// FIXME, this should be merged with the function below.
//
bool CUtil::Command(const CStdStringArray& arrArgs, bool waitExit)
{
#ifdef _DEBUG
  printf("Executing: ");
  for (size_t i=0; i<arrArgs.size(); i++)
    printf("%s ", arrArgs[i].c_str());
  printf("\n");
#endif

  pid_t child = fork();
  int n = 0;
  if (child == 0)
  {
    close(0);
    close(1);
    close(2);
    if (arrArgs.size() > 0)
    {
      char **args = (char **)alloca(sizeof(char *) * (arrArgs.size() + 3));
      memset(args, 0, (sizeof(char *) * (arrArgs.size() + 3)));
      for (size_t i=0; i<arrArgs.size(); i++)
        args[i] = (char *)arrArgs[i].c_str();
      execvp(args[0], args);
    }
  }
  else
  {
    if (waitExit) waitpid(child, &n, 0);
  }

  return (waitExit) ? (WEXITSTATUS(n) == 0) : true;
}

bool CUtil::SudoCommand(const CStdString &strCommand)
{
  CLog::Log(LOGDEBUG, "Executing sudo command: <%s>", strCommand.c_str());
  pid_t child = fork();
  int n = 0;
  if (child == 0)
  {
    close(0); // close stdin to avoid sudo request password
    close(1);
    close(2);
    CStdStringArray arrArgs;
    StringUtils::SplitString(strCommand, " ", arrArgs);
    if (arrArgs.size() > 0)
    {
      char **args = (char **)alloca(sizeof(char *) * (arrArgs.size() + 3));
      memset(args, 0, (sizeof(char *) * (arrArgs.size() + 3)));
      args[0] = (char *)"/usr/bin/sudo";
      args[1] = (char *)"-S";
      for (size_t i=0; i<arrArgs.size(); i++)
      {
        args[i+2] = (char *)arrArgs[i].c_str();
      }
      execvp("/usr/bin/sudo", args);
    }
  }
  else
    waitpid(child, &n, 0);

  return WEXITSTATUS(n) == 0;
}
#endif
