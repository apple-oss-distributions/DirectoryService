/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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
 * @header CLocalPluginNode
 */


#ifndef _CDSLocalPluginNode_
#define _CDSLocalPluginNode_	1

#include "CDSLocalPlugin.h"
#include "DSMutexSemaphore.h"
#include "DSEventSemaphore.h"

#if FILE_ACCESS_INDEXING
#include <sqlite3.h>
#include <map>

typedef struct _sIndexMapping			IndexMapping;
typedef map<string, IndexMapping *>		LocalNodeIndexMap;
typedef LocalNodeIndexMap::iterator		LocalNodeIndexMapI;

#endif

class CDSLocalPluginNode
{
	public:
								CDSLocalPluginNode( CFStringRef inNodeDirFilePath, CDSLocalPlugin* inPlugin );
		virtual					~CDSLocalPluginNode( void );
	
		void					CloseDatabase( void );

		tDirStatus				GetRecords( CFStringRef inNativeRecType, CFArrayRef inPatternsToMatch,
									CFStringRef inAttrTypeToMatch, tDirPatternMatch inPatternMatch, bool inAttrInfoOnly,
									unsigned long maxRecordsToGet, CFMutableArrayRef recordsArray, bool useLongNameAlso = false, CFStringRef* outRecFilePath = NULL );
		CFStringRef				CreateFilePathForRecord( CFStringRef inNativeRecType, CFStringRef inRecordName );
		tDirStatus				CreateDictionaryForRecord( CFStringRef inNativeRecType, CFStringRef inRecordName,
									CFMutableDictionaryRef* outMutableRecordDict, CFStringRef* outRecordFilePath );
		tDirStatus				CreateDictionaryForNewRecord( CFStringRef inNativeRecType, CFStringRef inRecordName,
									CFMutableDictionaryRef* outMutableRecordDict, CFStringRef* outRecordFilePath );
		tDirStatus				DeleteRecord( CFStringRef inRecordFilePath, CFStringRef inNativeRecType,
									CFStringRef inRecordName, CFMutableDictionaryRef inMutableRecordAttrsValues );
		tDirStatus				FlushRecord( CFStringRef inRecordFilePath, CFStringRef inRecordType,
									CFDictionaryRef inRecordDict );
		tDirStatus				AddAttributeToRecord( CFStringRef inNativeAttrType, CFStringRef inNativeRecType,
									CFTypeRef inAttrValue, CFMutableDictionaryRef inMutableRecordAttrsValues );
		tDirStatus				AddAttributeValueToRecord( CFStringRef inNativeAttrType, CFStringRef inNativeRecType,
									CFTypeRef inAttrValue, CFMutableDictionaryRef inMutableRecordAttrsValues );
		tDirStatus				RemoveAttributeFromRecord( CFStringRef inNativeAttrType, CFStringRef inNativeRecType,
									CFMutableDictionaryRef inMutableRecordAttrsValues );
		tDirStatus				RemoveAttributeValueFromRecordByCRC( CFStringRef inNativeAttrType,
									CFStringRef inNativeRecType, CFMutableDictionaryRef inMutableRecordAttrsValues,
									unsigned long inCRC );
		tDirStatus				ReplaceAttributeValueInRecordByCRC( CFStringRef inNativeAttrType,
									CFStringRef inNativeRecType, CFMutableDictionaryRef inMutableRecordAttrsValues,
									unsigned long inCRC, CFTypeRef inNewValue );
		tDirStatus				ReplaceAttributeValueInRecordByIndex( CFStringRef inNativeAttrType,
									CFStringRef inNativeRecType, CFMutableDictionaryRef inMutableRecordAttrsValues,
									unsigned long inIndex, CFStringRef inNewValue );
		tDirStatus				SetAttributeValuesInRecord( CFStringRef inNativeAttrType, CFStringRef inNativeRecType,
									CFMutableDictionaryRef inMutableRecordAttrsValues,
									CFMutableArrayRef inMutableAttrValues );
		tDirStatus				GetAttributeValueByCRCFromRecord( CFStringRef inNativeAttrType,
									CFMutableDictionaryRef inMutableRecordAttrsValues, unsigned long inCRC,
									CFTypeRef* outAttrValue );
		tDirStatus				GetAttributeValueByIndexFromRecord( CFStringRef inNativeAttrType,
									CFMutableDictionaryRef inMutableRecordAttrsValues, unsigned long inIndex,
									CFTypeRef* outAttrValue );
		bool					IsValidRecordName( CFStringRef inRecordName, CFStringRef inNativeRecType );
		void					FlushRecordCache();
		
		CFArrayRef				CreateAllRecordTypesArray();
		bool					WriteAccessAllowed(	CFDictionaryRef inNodeDict,
												    CFStringRef inNativeRecType,
												    CFStringRef inRecordName,
												    CFStringRef inNativeAttribute,
												    CFArrayRef	inWritersAccessRecord = NULL,
												    CFArrayRef	inWritersAccessAttribute = NULL,
												    CFArrayRef	inWritersGroupAccessRecord = NULL,
												    CFArrayRef	inWritersGroupAccessAttribute = NULL );
		bool					WriteAccessAllowed(	CFStringRef inAuthedUserName,
													CFNumberRef inEffectiveUIDNumber,
													CFStringRef inNativeRecType,
													CFStringRef inRecordName,
													CFStringRef inNativeAttribute,
													CFArrayRef	inWritersAccessRecord = NULL,
													CFArrayRef	inWritersAccessAttribute = NULL,
													CFArrayRef	inWritersGroupAccessRecord = NULL,
													CFArrayRef	inWritersGroupAccessAttribute = NULL );
		bool					WriteAccessAllowed( CFStringRef inAuthedUserName,
													uid_t inEffectiveUID,
													CFStringRef inNativeRecType,
													CFStringRef inRecordName,
													CFStringRef inNativeAttribute,
													CFArrayRef	inWritersAccessRecord = NULL,
													CFArrayRef	inWritersAccessAttribute = NULL,
													CFArrayRef	inWritersGroupAccessRecord = NULL,
													CFArrayRef	inWritersGroupAccessAttribute = NULL );
		bool					IsLocalNode();
		uint32_t				GetModValue( void );
		
	private:
		
		bool					RecordMatchesCriteria( CFDictionaryRef inRecordDict, CFArrayRef inPatternsToMatch,
									CFStringRef inNativeAttrTypeToMatch, tDirPatternMatch inPatternMatch );
		bool					RecordMatchesCriteriaTestString( CFStringRef inAttributeString, CFArrayRef inPatternsToMatch,
									CFStringRef inNativeAttrTypeToMatch, tDirPatternMatch inPatternMatch, bool inNeedLowercase );
		bool					RecordMatchesCriteriaTestData( CFDataRef inAttributeData, CFArrayRef inPatternsToMatch,
									CFStringRef inNativeAttrTypeToMatch, tDirPatternMatch inPatternMatch, bool inNeedLowercase );
		void					UpdateModValue(void);
		void					RemoveShadowHashFilesIfNecessary( CFStringRef inNativeAttrType,
									CFMutableDictionaryRef inMutableRecordAttrsValues );
		void					RemoveShadowHashFilesWithPath( CFStringRef inPath );
		CFStringRef				GetShadowHashFilePath( CFStringRef inNativeAttrType,
									CFMutableDictionaryRef inMutableRecordAttrsValues );
		void					GetDataFromEnabledOrDisabledKerberosTag( CFStringRef inAuthAuthority, char **outPrincipal, char **outRealm );
		CFStringRef				GetKerberosTagIfPresent( CFArrayRef inAttrValues );
		CFStringRef				GetDisabledKerberosTagIfPresent( CFArrayRef inAttrValues );
		bool					ArrayContainsShadowHashOrLocalCachedUser( CFArrayRef inAttrValues );
		CFStringRef				GetTagInArray( CFArrayRef inAttrValues, CFStringRef inTag );
		tDirStatus				RenameShadowHashFiles( CFStringRef inCurrentPath, CFStringRef inNewGUIDString );
		tDirStatus				AttributeValueMatchesUserAlias( CFStringRef inNativeRecType, CFStringRef inNativeAttrType, 
															    CFTypeRef inAttrValue, CFDictionaryRef inRecordDict );
		void					SetKerberosTicketsEnabledOrDisabled( CFMutableArrayRef inMutableAttrValues );
		void					SetPrincipalStateInLocalRealm(char* principalName, const char *realmName, bool enabled);
		CFDataRef				CreateCFDataFromFile( const char *filename, size_t inLength );
		
#if FILE_ACCESS_INDEXING
		void					AddIndexMapping( const char *inStdRecordType, ... );

		void					LoadFileAccessIndex( void );

		void					AddRecordIndex( const char *inStdRecordType, const char *inFileName );
		void					DeleteRecordIndex( const char *inStdRecordType, const char *inFileName );
	
		char**					GetFileAccessIndex(CFStringRef inNativeRecType, tDirPatternMatch inPatternMatch, CFStringRef inPatternToMatch, 
												   CFStringRef inNativeAttrToMatch, bool *outPreferIndex );
	
		int						sqlExecSync( const char *command, UInt32 commandLength = -1 );
	
		int						EnsureDirs( const char *inPath, mode_t inPathMode, mode_t inFinalMode );
		static void *			LoadIndexAsynchronously( void *inPtr );
		void					RemoveIndex( void );
		void					DatabaseCorrupt( void );
		static void				IndexObject( const void *inValue, void *inContext );

	private:
		DSMutexSemaphore			fDBLock;
		sqlite3*					mFileAccessIndexPtr;
		char*						mIndexPath;
		int32_t						mUseIndex;
		int32_t						mIndexLoading;
		LocalNodeIndexMap			mIndexMap;
		DSEventSemaphore			mIndexLoaded;
		bool						mProperShutdown;
		bool						mSafeBoot;
#endif
		
		CDSLocalPlugin*				mPlugin;
		DSMutexSemaphore			mOpenRecordsLock;
		DSMutexSemaphore			mRecordTypeLock;
		CFStringRef					mNodeDirFilePath;
		char						*mNodeDirFilePathCStr;
		CFStringRef					mRecordNameAttrNativeName;
		uint32_t					mModCounter;
};

#endif
