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
 * @header COSUtils
 */

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <syslog.h>

#include "COSUtils.h"

/*
	>***    [<date>.]<service-name>[.<level>][.<category>].log
	>***
	>*** Examples:
	>***
	>***    990727.FTP.1.errors.log
	>***    990727.HTTP.access.log
	>***    Apache.errors.log
	>***    990727.SMB.critical.log
	>***    AFP.log
	>***
	>*** Note: The date, level and category fields are optional.
*/

static const char	*appStrList [] =
{
	/* 01 */	"Frameworks/DirectoryService.framework/Resources/Plugins",
	/* 02 */	"DirectoryService",
	/* 03 */	"Plugins",
	/* 04 */	"Logs",
	/* 05 */	"server.log",
	/* 06 */	"error.log",
	/* 07 */	"debug.log",
	/* 08 */	"info.log",
	/* 09 */	"dsplug",
	/* 10 */	"DirectoryServices/PlugIns"
};

// $(NEXT_ROOT)$(SYSTEM_LIBRARY_DIR)/Frameworks/DirectoryService.framework/Resources/Plugins
// ---------------------------------------------------------------------------
//	* GetStringFromList ()
//
// ---------------------------------------------------------------------------

const char* COSUtils::GetStringFromList ( const uInt32 inListID, const sInt32 inIndex )
{
	static const char	*_sNoMatch	= "<STRING NOT FOUND>";
				 char	*pStr		= (char *)_sNoMatch;

	switch ( inListID )
	{
		case kAppStringsListID:
			if ( (inIndex - 1) < (sInt32)(sizeof (appStrList) / sizeof( char * )) )
			{
				pStr = (char *)appStrList[ inIndex - 1 ];
			}
			break;

		default:
			break;
	}

	return( pStr );

} // GetXndString

int dsTouch( const char* path )
{
	int		fd = -1;
	int		status = 0;
	
	if ( path )
	{
		fd = open( path, O_NOFOLLOW | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR );
		
		if ( fd >= 0 )
		{
			if ( close( fd ) )
				status = errno;
		}
		else
		{
			status = errno;
			syslog( LOG_ALERT, "WARNING - dsTouch: file was asked to be opened <%s>: (%s)\n", path, strerror(errno) );
		}
	}
	else
		status = EINVAL;
		
	return status;
}

int dsRemove( const char* path  )
{
	if ( unlink( path ) )
	{
		if ( errno != ENOENT )	// if the error wasn't that the file didn't already exist
		{
			syslog( LOG_ALERT, "WARNING - dsRemove: file was asked to be deleted that should be zero length but isn't! <%s> (%s)\n", path, strerror(errno) );

			return errno;
		}
	}

	return 0;
}


