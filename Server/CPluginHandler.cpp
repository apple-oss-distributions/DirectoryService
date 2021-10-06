/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*!
 * @header CPluginHandler
 */

#include <CoreFoundation/CFPlugIn.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>

#include "CPluginHandler.h"
#include "CPlugInList.h"
#include "ServerControl.h"
#include "DirServiceMain.h"
#include "CString.h"
#include "COSUtils.h"
#include "CLog.h"
#include "CServerPlugin.h"
#include "CDSPluginUtils.h"
#include "CCachePlugin.h"
#include "DSEventSemaphore.h"

// --------------------------------------------------------------------------------
//	* Globals
// --------------------------------------------------------------------------------

#warning VERIFY the version string for the static plugins before each software release
//KW not clear we use the version string for anything yet in a static plugin
//static plugin name and version strings
static const char *sStaticPluginList[ kNumStaticPlugins ][ 2 ] =
{
	{ "Cache",		"1.0" },
	{ "Configure",	"3.0" },
	{ "NetInfo",	"3.0" },
	{ "Local",		"1.1" },
	{ "LDAPv3",		"3.1" },
	{ "Search",		"3.1" },
	{ "BSD",		"2.0" }
};

extern	dsBool			gDSLocalOnlyMode;
extern	bool			gNetInfoPluginIsLoaded;
extern	dsBool			gDSInstallDaemonMode;
extern	CCachePlugin   *gCacheNode;
extern  DSEventSemaphore gKickCacheRequests;

//--------------------------------------------------------------------------------------------------
//	* CPluginHandler()
//
//--------------------------------------------------------------------------------------------------

CPluginHandler::CPluginHandler ( void ) : CInternalDispatchThread(kTSPlugInHndlrThread)
{
	fThreadSignature = kTSPlugInHndlrThread;
} // CPluginHandler



//--------------------------------------------------------------------------------------------------
//	* ~CPluginHandler()
//
//--------------------------------------------------------------------------------------------------

CPluginHandler::~CPluginHandler()
{
} // ~CPluginHandler


//--------------------------------------------------------------------------------------------------
//	* StartThread()
//
//--------------------------------------------------------------------------------------------------

void CPluginHandler::StartThread ( void )
{
	if ( this == nil ) throw((SInt32)eMemoryError);

	this->Resume();
} // StartThread



//--------------------------------------------------------------------------------------------------
//	* StopThread()
//
//--------------------------------------------------------------------------------------------------

void CPluginHandler::StopThread ( void )
{
	if ( this == nil ) throw((SInt32)eMemoryError);

	// Check that the current thread context is not our thread context
//	SignalIf_( this->IsCurrent() );

	SetThreadRunState( kThreadStop );		// Tell our thread to stop

} // StopThread



//--------------------------------------------------------------------------------------------------
//	* ThreadMain()
//
//--------------------------------------------------------------------------------------------------

SInt32 CPluginHandler::ThreadMain ( void )
{
	UInt32		uiPluginCnt		= 0;
	UInt32		statPluginCnt	= 0;
	SInt32		status			= eDSNoErr;

	if (gDSLocalOnlyMode) //1 is for Configure plugin and 3 is for Local plugin
	{
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[1][0],
														sStaticPluginList[1][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[3][0],
														sStaticPluginList[3][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		statPluginCnt = uiPluginCnt;

		DbgLog( kLogApplication, "%d Plugins processed.", uiPluginCnt );

		DbgLog( kLogApplication, "Initializing static plugins." );
		gPlugins->InitPlugIns(kStaticPlugin);		
	}
	else if (gDSInstallDaemonMode) //0-Cache, 1-Configure, 3-Local, 5-Search, 6-BSD plugins
	{
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[0][0],
														sStaticPluginList[0][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[1][0],
														sStaticPluginList[1][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[3][0],
														sStaticPluginList[3][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[5][0],
														sStaticPluginList[5][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[6][0],
														sStaticPluginList[6][1]);
		if (status == eDSNoErr)
		{
			uiPluginCnt++;
		}
		statPluginCnt = uiPluginCnt;

		DbgLog( kLogApplication, "%d Plugins processed.", uiPluginCnt );

		DbgLog( kLogApplication, "Initializing static plugins." );
		gPlugins->InitPlugIns(kStaticPlugin);		
	}
	else
	{
		//process the static plugin modules
		for (UInt32 iPlugin = 0; iPlugin < kNumStaticPlugins; iPlugin++)
		{	
			if (( (iPlugin == 2) && gNetInfoPluginIsLoaded) || (iPlugin != 2) )
			{
				status = CServerPlugin::ProcessStaticPlugin(	sStaticPluginList[iPlugin][0],
																sStaticPluginList[iPlugin][1]);
				if (status == eDSNoErr)
				{
					uiPluginCnt++;
				}
			}
		}
		statPluginCnt = uiPluginCnt;
		
		DbgLog( kLogApplication, "%d Plugins processed.", uiPluginCnt );

		DbgLog( kLogApplication, "Initializing static plugins." );
		gPlugins->InitPlugIns(kStaticPlugin);

        DbgLog( kLogApplication, "Waiting on Cache node initialization" );
        gKickCacheRequests.WaitForEvent();
        DbgLog( kLogApplication, "Cache node initialization - succeeded" );
		
		uiPluginCnt = LoadPlugins(uiPluginCnt);
	}
	
	if ( (uiPluginCnt - statPluginCnt) == 0 )
	{
		//SrvrLog( kLogApplication, "*** WARNING: No Plugins loaded ***" );
		ErrLog( kLogApplication, "*** WARNING: No Plugins loaded ***" );
	}
	else
	{
		DbgLog( kLogApplication, "%d Plugins loaded.", uiPluginCnt - statPluginCnt );

		DbgLog( kLogApplication, "Initializing loaded plugins." );
		gPlugins->InitPlugIns(kAppleLoadedPlugin);
	}

	return( 0 );

} // ThreadMain


//--------------------------------------------------------------------------------------------------
//	* LoadPlugins ()
//
//--------------------------------------------------------------------------------------------------

UInt32 CPluginHandler::LoadPlugins ( UInt32 inCount )
{
	SInt32				status	= eDSNoErr;
	UInt32				uiCount	= inCount;
	CString				cSubPath( COSUtils::GetStringFromList( kAppStringsListID, kStrPlugInsFolder ) );
	CString				cOtherSubPath( COSUtils::GetStringFromList( kAppStringsListID, kStrOtherPlugInsFolder ) );
	CString				cPlugExt( COSUtils::GetStringFromList( kAppStringsListID, kStrPluginExtension ) );
	CFStringRef			sType	= nil;
	char				string	[ PATH_MAX ];


	//KW now we need to know which plugins are lazily loaded and which
	//are required now to be loaded
	
	//Retrieve all the Apple developed plugins
	sType = ::CFStringCreateWithCString( kCFAllocatorDefault, cPlugExt.GetData(), kCFStringEncodingMacRoman );

	CFURLRef	urlPath = 0;
	CFStringRef	sBase, sPath;
	CFArrayRef	aBundles;

	// Append the subpath.
	sBase = CFSTR( "/System/Library" );
	sPath = ::CFStringCreateWithFormat( kCFAllocatorDefault, NULL, CFSTR( "%@/%s" ), sBase, cSubPath.GetData() );
	sBase = nil;

	::CFStringGetCString( sPath, string, sizeof( string ), kCFStringEncodingMacRoman );
	DbgLog( kLogApplication, "Checking for plugins in:" );
	DbgLog( kLogApplication, "  %s", string );

	// Convert it back into a CFURL.
	urlPath = ::CFURLCreateWithFileSystemPath( kCFAllocatorDefault, sPath, kCFURLPOSIXPathStyle, true );

	CFDebugLog( kLogApplication, "LoadPlugins:CFURLCreateWithFileSystemPath called on path <%@>", sPath );

	::CFRelease( sPath );
	sPath = nil;

	// Enumerate all the plugins in this system directory.
	aBundles = ::CFBundleCopyResourceURLsOfTypeInDirectory( urlPath, sType, NULL );
	::CFRelease( urlPath );
	urlPath = nil;

	CFDebugLog( kLogApplication, "LoadPlugins:CFBundleCopyResourceURLsOfTypeInDirectory called on urlPath" );

	if ( aBundles != nil )
	{
		register CFIndex bundleCount = CFArrayGetCount( aBundles );
		for ( register CFIndex j = 0; j < bundleCount; ++j )
		{
CFDebugLog( kLogApplication, "LoadPlugins:CServerPlugin::ProcessURL about to be called on index <%d>", j );
			status = CServerPlugin::ProcessURL( (CFURLRef)::CFArrayGetValueAtIndex( aBundles, j ) );
			if ( status == eDSNoErr )
			{
				uiCount++;
			}
			else
				SrvrLog( kLogApplication, "\tError loading plugin, see DirectoryService.error.log for details" );
		}
	
		::CFRelease( aBundles );
		aBundles = nil;
	}

	if ( sType != nil )
	{
		::CFRelease( sType );
		sType = nil;
	}

	//Now retrieve any Third party developed plugins
	sType = ::CFStringCreateWithCString( kCFAllocatorDefault, cPlugExt.GetData(), kCFStringEncodingMacRoman );

	// Append the subpath.
	sBase = CFSTR( "/Library" );
	sPath = ::CFStringCreateWithFormat( kCFAllocatorDefault, NULL, CFSTR( "%@/%s" ), sBase, cOtherSubPath.GetData() );
	sBase = nil;

	::CFStringGetCString( sPath, string, sizeof( string ), kCFStringEncodingMacRoman );
	DbgLog( kLogApplication, "Checking for plugins in:" );
	DbgLog( kLogApplication, "  %s", string );

	// Convert it back into a CFURL.
	urlPath = ::CFURLCreateWithFileSystemPath( kCFAllocatorDefault, sPath, kCFURLPOSIXPathStyle, true );
	::CFRelease( sPath );
	sPath = nil;

	// Enumerate all the plugins in this system directory.
	aBundles = ::CFBundleCopyResourceURLsOfTypeInDirectory( urlPath, sType, NULL );
	::CFRelease( urlPath );
	urlPath = nil;

	if ( aBundles != nil )
	{
		register CFIndex j = ::CFArrayGetCount( aBundles );
		while ( j-- )
		{
			status = CServerPlugin::ProcessURL( (CFURLRef)::CFArrayGetValueAtIndex( aBundles, j ) );
			if ( status == eDSNoErr )
			{
				uiCount++;
			}
			else
				SrvrLog( kLogApplication, "\tError loading 3rd party plugin, see DirectoryService.error.log for details" );
		}

		::CFRelease( aBundles );
		aBundles = nil;
	}

	if ( sType != nil )
	{
		::CFRelease( sType );
		sType = nil;
	}

	return( uiCount );

} // LoadPlugins

