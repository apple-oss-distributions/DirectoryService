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
 * @header CNodeList
 */

#ifndef __CNodeList_h__
#define __CNodeList_h__	1

#include "DirServicesTypes.h"
#include "PrivateTypes.h"
#include "DSMutexSemaphore.h"
#include "PluginData.h"


// Typedefs --------------------------------------------------------------------

class	CServerPlugin;

// Classes ---------------------------------------------------------------------

class CNodeList {

public:

typedef struct sTreeNode
{
   	char			*fNodeName;
	tDataList		*fDataListPtr;
	CServerPlugin	*fPlugInPtr;
	uInt32			fPlugInToken;
	eDirNodeType	fType;
   	sTreeNode		*left;
   	sTreeNode		*right;
} sTreeNode;

enum {
	kBuffFull		= -128,
	kBuffTooSmall	= -129,
	kDupNodeErr		= -130
};

public:
					CNodeList			( void );
	virtual		   ~CNodeList			( void );

	void			RegisterAll			( void );
	uInt32			GetNodeCount		( void );
	uInt32			GetNodeChangeToken 	( void );
	void			CountNodes			( uInt32 *outCount );
	bool		 	IsPresent			( const char *inStr, eDirNodeType inType );
	bool		 	GetPluginHandle		( const char *inStr, CServerPlugin **outPlugInPtr );
	CServerPlugin*	GetPluginPtr		( sTreeNode* nodePtr );
	
	sInt32			GetNodes			( char *inStr, tDirPatternMatch inMatch, tDataBuffer *inBuff );

	sInt32		   	AddNode				( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	bool			DeleteNode			( char *inStr );

	sInt32		   	BuildNodeListBuff	( sGetDirNodeList *inData );

	const char*		GetDelimiter		( void ) { return( "~" ); };

	void			WaitForLocalNode		( void );
	void			WaitForConfigureNode	( void );
	void			WaitForDHCPLDAPv3Init	( void );
	void			WaitForAuthenticationSearchNode	( void );

protected:
	// Protected member functions
	sInt32		DeleteTree				( sTreeNode **inTree );	// called by the destructor
	bool		DeleteNodeFromTree		( char *inStr, sTreeNode  *inTree );

	sInt32		AddNodePathToTDataBuff	( tDataList *inPtr, tDataBuffer *inBuff );

private:
	// Private member functions
	void		Count					( sTreeNode *inTree, uInt32 *outCount );
	sInt32		DoGetNode				( sTreeNode *inTree, char *inStr, tDirPatternMatch inMatch, tDataBuffer *inBuff, sTreeNode **outNodePtr );
	void		Register				( sTreeNode *inTree );
	sInt32		CompareString			( const char *inStr_1, const char *inStr_2 );

	sInt32		DoBuildNodeListBuff		( sTreeNode *inTree, tDataBuffer *outData, uInt32 *outCount );

	sInt32	   	AddLocalNode					( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddNodeToTree					( sTreeNode **inTree, const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddAuthenticationSearchNode		( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddContactsSearchNode			( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddNetworkSearchNode			( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddConfigureNode				( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );
	sInt32	   	AddDHCPLDAPv3Node				( const char *inStr, tDataList *inListPtr, eDirNodeType inType, CServerPlugin *inPlugInPtr, uInt32 inToken );

	bool	 	GetLocalNode					( CServerPlugin **outPlugInPtr );
	bool	 	GetLocalNode					( tDataList **outListPtr );

	bool	 	GetAuthenticationSearchNode		( CServerPlugin **outPlugInPtr );
	bool	 	GetAuthenticationSearchNode		( tDataList **outListPtr );

	bool	 	GetContactsSearchNode			( CServerPlugin **outPlugInPtr );
	bool	 	GetContactsSearchNode			( tDataList **outListPtr );

	bool	 	GetNetworkSearchNode			( CServerPlugin **outPlugInPtr );
	bool	 	GetNetworkSearchNode			( tDataList **outListPtr );

	void		WaitForContactsSearchNode		( void );
	void		WaitForNetworkSearchNode		( void );

	// Private data members
	sTreeNode		   *fTreePtr;
	sTreeNode		   *fLocalNode;
	sTreeNode		   *fConfigureNode;
	sTreeNode		   *fAuthenticationSearchNode;
	sTreeNode		   *fContactsSearchNode;
	sTreeNode		   *fNetworkSearchNode;
	sTreeNode		   *fLocalHostedNodes;
	sTreeNode		   *fDefaultNetworkNodes;
	uInt32				fCount;
	uInt32				fNodeChangeToken;
    bool				bDHCPLDAPv3InitComplete;

	DSMutexSemaphore		fMutex;
	DSMutexSemaphore		fWaitForAuthenticationSN;
	DSMutexSemaphore		fWaitForContactsSN;
	DSMutexSemaphore		fWaitForNetworkSN;
	DSMutexSemaphore		fWaitForLN;
	DSMutexSemaphore		fWaitForConfigureN;
	DSMutexSemaphore		fWaitForDHCPLDAPv3InitFlag;
};

#endif
