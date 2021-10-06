/*
 * Copyright (c) 2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*!
 * @header CSearchNode
 * Implements the search policies.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>			//used for mkdir and stat

#include <Security/Authorization.h>

#include "DirServices.h"
#include "DirServicesUtils.h"
#include "DirServicesConst.h"

#include "SharedConsts.h"
#include "CSharedData.h"
#include "PrivateTypes.h"
#include "DSUtils.h"
#include "CAttributeList.h"
#include "CPlugInRef.h"
#include "CBuff.h"
#include "CDataBuff.h"
#include "CRecTypeList.h"

#include "CSearchNode.h"
#include "ServerModuleLib.h"
#include "DSUtils.h"
#include "PluginData.h"
#include "DSCThread.h"
#include "DSEventSemaphore.h"
#include "CAliases.h"
#include "CContinue.h"

// Globals ---------------------------------------------------------------------------

static CPlugInRef		 	*gSNNodeRef				= nil;
static CContinue		 	*gSNContinue			= nil;
static DSEventSemaphore		*gKickSearchRequests	= nil;
static CSearchNode			*gSearchNode			= nil;

static	const	FourCharCode	kSearchNodeInfo	= 'SnIn';
static	const	uInt32			kBuffPad		= 16;

extern "C" {
CFUUIDRef ModuleFactoryUUID = CFUUIDGetConstantUUIDWithBytes ( NULL, \
								0x96, 0xE1, 0xAB, 0xD6, 0xAE, 0xA6, 0x12, 0x26, \
								0xA6, 0x10, 0x00, 0x05, 0x02, 0xC1, 0xC7, 0x36 );

}

static CDSServerModule* _Creator ( void )
{
	return( new CSearchNode );
}

CDSServerModule::tCreator CDSServerModule::sCreator = _Creator;


static void DoSNPINetworkChange(CFRunLoopTimerRef timer, void *info);
void DoSNPINetworkChange(CFRunLoopTimerRef timer, void *info)
{
	if ( info != nil )
	{
		((CSearchNode *)info)->ReDiscoverNetwork();
	}
}// DoSNPINetworkChange


CFStringRef NetworkChangeSNPICopyStringCallback( const void *item );
CFStringRef NetworkChangeSNPICopyStringCallback( const void *item )
{
	return CFSTR("NetworkChangeinSNPI");
}

// --------------------------------------------------------------------------------
//	* CSearchNode ()
// --------------------------------------------------------------------------------

CSearchNode::CSearchNode ( void )
{
	fDirRef					= 0;
	fState					= kUnknownState;
	fToken					= 0;
	pSearchConfigList		= nil;
	fServerRunLoop			= nil;
	fTransitionCheckTime	= 0;

	if ( gSNNodeRef == nil )
	{
		gSNNodeRef = new CPlugInRef( CSearchNode::ContextDeallocProc );
	}

	if ( gSNContinue == nil )
	{
		gSNContinue = new CContinue( CSearchNode::ContinueDeallocProc );
	}

	if ( gKickSearchRequests == nil )
	{
		gKickSearchRequests = new DSEventSemaphore();
	}
	
	gSearchNode = this;

	::dsOpenDirService( &fDirRef ); //don't check the return since we are direct dispatch inside the daemon

} // CSearchNode


// --------------------------------------------------------------------------------
//	* ~CSearchNode ()
// --------------------------------------------------------------------------------

CSearchNode::~CSearchNode ( void )
{
	sSearchConfig  	   *pConfig			= nil;
	sSearchConfig  	   *pDeleteConfig	= nil;

	//need to cleanup the struct list ie. the internals
	pConfig = pSearchConfigList;
	while (pConfig != nil)
	{
		pDeleteConfig = pConfig;
		pConfig = pConfig->fNext;		//assign to next BEFORE deleting current
		CleanSearchConfigData( pDeleteConfig );
		free( pDeleteConfig );
		pDeleteConfig = nil;
	}
	pSearchConfigList = nil;

	if (fDirRef != 0)
	{
		::dsCloseDirService( fDirRef );
	}

} // ~CSearchNode


// --------------------------------------------------------------------------------
//	* Validate ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::Validate ( const char *inVersionStr, const uInt32 inSignature )
{
	fToken = inSignature;

	return( eDSNoErr );
} // Validate


// --------------------------------------------------------------------------------
//	* PeriodicTask ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::PeriodicTask ( void )
{
	return( eDSNoErr );
} // PeriodicTask


// --------------------------------------------------------------------------------
//	* Initialize ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::Initialize ( void )
{
	sInt32			siResult				= eDSNoErr;
	sInt32			result					= eDSNoErr;
	sInt32			addLDAPResult			= eSearchPathNotDefined;
	tDataList	   *aNodeName				= nil;
	sSearchConfig  *aSearchConfig			= nil;
	sSearchConfig  *lastSearchConfig		= nil;
	uInt32			index					= 0;
	uInt32			aSearchConfigType		= 0;
	char		   *aSearchNodeName			= nil;
	char		   *aSearchConfigFilePrefix	= nil;
	sSearchList	   *aSearchNodeList			= nil;
	sSearchList	   *autoSearchNodeList		= nil;
	CConfigs	   *aConfigFromXML			= nil;
	uInt32			aSearchPolicy			= 0;
	eDirNodeType	aDirNodeType			= kUnknownNodeType;

	try
	{
		//verify the dirRef here and only open a new one if required
		//can't believe we ever need a new one since we are direct dispatch inside the daemon
		siResult = ::dsVerifyDirRefNum(fDirRef);
		if (siResult != eDSNoErr)
		{
			// Get a directory services reference as a member variable
			siResult = ::dsOpenDirService( &fDirRef );
			if ( siResult != eDSNoErr ) throw( siResult );
		}

		//KW here we create the multiple search configs that CAN be used by this search node ie. three of them now
		//--might in the future determine exactly how these get initialized but for now they are hard coded
		//--to three separate configs:one for Auth, one for Contacts, and one for Default Network
		//note that the first two are setup the same way and the Default Network one is separate outside of the for loop
		//the rest of the code can easily deal with any number of configs
		for (index=0; index<2; index++)
		{
			if (index == 0)
			{
				CShared::LogIt( 0x0F, "Setting Authentication Search Node Configuraton" );
				aSearchConfigType	= eDSAuthenticationSearchNodeName;
				aDirNodeType		= kSearchNodeType;
			}
			else if (index == 1)
			{
				CShared::LogIt( 0x0F, "Setting Contacts Search Node Configuraton" );
				aSearchConfigType 	= eDSContactsSearchNodeName;
				aDirNodeType		= kContactsSearchNodeType;
			}

			fMutex.Wait();
			aSearchConfig = FindSearchConfigWithKey(aSearchConfigType);
			if (aSearchConfig != nil)  //checking if we are simply re-entrying intialize
			//so don't want to ignore what is already set-up but do want to possibly switch the search policy
			{
				aConfigFromXML 			= aSearchConfig->pConfigFromXML;
				aSearchNodeName			= aSearchConfig->fSearchNodeName;
				aSearchConfigFilePrefix	= aSearchConfig->fSearchConfigFilePrefix;
			}
			else
			{
				if (index == 0)
				{
					aSearchNodeName		= (char *) ::calloc(sizeof(kstrAuthenticationNodeName) + 1, sizeof(char));
					::strcpy(aSearchNodeName, kstrAuthenticationNodeName);
					aSearchConfigFilePrefix = (char *) ::calloc(sizeof(kstrAuthenticationConfigFilePrefix) + 1, sizeof(char));
					::strcpy(aSearchConfigFilePrefix, kstrAuthenticationConfigFilePrefix);
				}
				else
				{
					aSearchNodeName		= (char *) ::calloc(sizeof(kstrContactsNodeName) + 1, sizeof(char));
					::strcpy(aSearchNodeName, kstrContactsNodeName);
					aSearchConfigFilePrefix = (char *) ::calloc(sizeof(kstrContactsConfigFilePrefix) + 1, sizeof(char));
					::strcpy(aSearchConfigFilePrefix, kstrContactsConfigFilePrefix);
				}
			}
			fMutex.Signal();
			
			//this is where the XML config file comes from
			if ( aConfigFromXML == nil )
			{
				aConfigFromXML = new CConfigs();
				if ( aConfigFromXML != nil )
				{
					result = aConfigFromXML->Init( aSearchConfigFilePrefix, aSearchPolicy );
					if ( result != eDSNoErr ) //use default if error
					{
						aSearchPolicy = 1; //automatic is the default
					}
				}
				else
				{
					aSearchPolicy = 1; //automatic is the default
				}
			}
			else if (aSearchConfig != nil) //retain the same search policy for re-entry
			{
				aSearchPolicy = aSearchConfig->fSearchPolicy;
			}
		
			switch ( aSearchPolicy )
			{
				case kCustomSearchPolicy:
					CShared::LogIt( 0x0F, "Setting search policy to Custom search" );
					aSearchNodeList = aConfigFromXML->GetCustom();
					
					//if custom list was nil we go ahead anyways with local only
					//local policy nodes always added in regardless
					siResult = AddLocalNodesAsFirstPaths(&aSearchNodeList);
					break;

				case kLocalSearchPolicy:
					CShared::LogIt( 0x0F, "Setting search policy to Local search" );
					//local policy call
					siResult = AddLocalNodesAsFirstPaths(&aSearchNodeList);
					break;

				case kNetInfoSearchPolicy:
				default:
					CShared::LogIt( 0x0F, "Setting search policy to NetInfo default" );
					if (autoSearchNodeList == nil)
					{
						siResult = DoNetInfoDefault(&aSearchNodeList);
						autoSearchNodeList = DupSearchListWithNewRefs(aSearchNodeList);
					}
					else
					{
						aSearchNodeList = DupSearchListWithNewRefs(autoSearchNodeList);
					}
					break;
			} // switch on aSearchPolicy
			
			if (siResult == eDSNoErr)
			{
				if (aSearchPolicy == kNetInfoSearchPolicy)
				{
					//get the default LDAP search paths if they are present
					//don't check status on return as continuing on anyways
					//don't add on to the custom path
					if ( aConfigFromXML->IsDHCPLDAPEnabled() )
					{
						addLDAPResult = AddDefaultLDAPNodesLast(&aSearchNodeList);
					}
				}
				
				if (aSearchConfig != nil) //clean up the old search list due to re-entry and add in the new
				{
					//flush the old search path list
					CleanSearchListData( aSearchConfig->fSearchNodeList );
					aSearchConfig->fSearchNodeList	= aSearchNodeList;
					aSearchConfig->fSearchPolicy	= aSearchPolicy;
				}
				else
				{
					//now get this search config
					aSearchConfig	= MakeSearchConfigData(	aSearchNodeList,
															aSearchPolicy,
															aConfigFromXML,
															aSearchNodeName,
															aSearchConfigFilePrefix,
															aDirNodeType,
															aSearchConfigType);
					//now put aSearchConfig in the list
					AddSearchConfigToList(aSearchConfig);
				}
	
				//set the indicator file
				if (addLDAPResult == eSearchPathNotDefined)
				{
					SetSearchPolicyIndicatorFile(aSearchConfigType, aSearchPolicy);
				}
				else //DHCP LDAP nodes added so make sure indicator file shows a custom policy
				{
					SetSearchPolicyIndicatorFile(aSearchConfigType, kCustomSearchPolicy);
				}
				addLDAPResult = eSearchPathNotDefined;
	
				aSearchNodeList			= nil;
				aSearchPolicy			= 0;
				aConfigFromXML			= nil;
				aSearchNodeName			= nil;
				aSearchConfigFilePrefix	= nil;
				lastSearchConfig		= aSearchConfig;
				aSearchConfig			= nil;
				
				// make search node active
				fState = kUnknownState;
				fState += kInitalized;
				fState += kActive;
		
				CSearchNode::WakeUpRequests();
			}

		} //for loop over search node indices
		
		//clean up the cached auto search list if it exists
		if ( (autoSearchNodeList != nil) && (lastSearchConfig != nil) )
		{
			CleanSearchListData( autoSearchNodeList );
		}

		
		{  //Default Network Search Policy
			CShared::LogIt( 0x0F, "Setting Detault Network Search Node Configuraton" );
			aSearchConfigType	= eDSNetworkSearchNodeName;
			aDirNodeType		= kNetworkSearchNodeType;
			aSearchPolicy		= kCustomSearchPolicy;
			
			fMutex.Wait();
			aSearchConfig = FindSearchConfigWithKey(aSearchConfigType);
			if (aSearchConfig != nil)  //checking if we are simply re-entrying intialize
			//so don't want to ignore what is already set-up
			{
				aConfigFromXML 			= aSearchConfig->pConfigFromXML;
				aSearchNodeName			= aSearchConfig->fSearchNodeName;
				aSearchConfigFilePrefix	= aSearchConfig->fSearchConfigFilePrefix;  //should be NULL
			}
			else
			{
				aSearchNodeName		= (char *) ::calloc( 1, sizeof(kstrNetworkNodeName) + 1 );
				::strcpy(aSearchNodeName, kstrNetworkNodeName);
				aSearchConfigFilePrefix = NULL;
				//this is where the XML config file comes from but is unused by this search node
				//however, we need the class for functions within it
				if ( aConfigFromXML == nil )
				{
					aConfigFromXML = new CConfigs();
					//if aConfigFromXML is nil then it is checked for later and not used
				}
			}
			fMutex.Signal();
			
			//register any default network nodes if any can be determined here
			//siResult = DoDefaultNetworkNodes(&aSearchNodeList);??
			//likely that since they are built automatically that there will be none added here
			//so the search policy list for now will be nil
			//the DS daemon knows the list and the search node can get the list when it needs it
			//ie. lazily get it when a call comes in needing it ie. when the Default Network node is actually opened
			
			if (aSearchConfig != nil) //clean up the old search list due to re-entry and add in the new
			{
				//flush the old search path list
				CleanSearchListData( aSearchConfig->fSearchNodeList );
				aSearchConfig->fSearchNodeList	= aSearchNodeList;
				aSearchConfig->fSearchPolicy	= aSearchPolicy;
			}
			else
			{
				//now get this search config
				aSearchConfig	= MakeSearchConfigData(	aSearchNodeList,
														aSearchPolicy,
														aConfigFromXML,
														aSearchNodeName,
														aSearchConfigFilePrefix,
														aDirNodeType,
														aSearchConfigType);
				//now put aSearchConfig in the list
				AddSearchConfigToList(aSearchConfig);
			}
		} //Default Network Search Policy end
		
	}

	catch( sInt32 err )
	{
		siResult = err;
		fState = kUnknownState;
		fState += kFailedToInit;
	}

	fMutex.Wait();
	aSearchConfig = pSearchConfigList;
	while (aSearchConfig != nil) //register all the search nodes that were successfully created
	{
		aNodeName = ::dsBuildFromPathPriv( aSearchConfig->fSearchNodeName, "/" );
		if ( aNodeName != nil )
		{
			DSRegisterNode( fToken, aNodeName, aSearchConfig->fDirNodeType );

			::dsDataListDeallocatePriv( aNodeName );
			free( aNodeName );
			aNodeName = nil;
		}
		aSearchConfig = aSearchConfig->fNext;
	}
	fMutex.Signal();
	
	return( siResult );

} // Initialize


// --------------------------------------------------------------------------------
//	* SwitchSearchPolicy ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode:: SwitchSearchPolicy ( uInt32 inSearchPolicy, sSearchConfig *inSearchConfig )
{
	sInt32			siResult		= eDSNoErr;
	sInt32			addLDAPResult	= eSearchPathNotDefined;

	fMutex.Wait();
	
	try
	{
			//this is where the XML config file comes from
			if ( inSearchConfig->pConfigFromXML == nil )
			{
				inSearchConfig->pConfigFromXML = new CConfigs();
				if ( inSearchConfig->pConfigFromXML  == nil ) throw( (sInt32)eDSPlugInConfigFileError );
				//don't use the search policy from the XML config file
				//however, need to init with the file
				siResult = inSearchConfig->pConfigFromXML->Init( inSearchConfig->fSearchConfigFilePrefix, inSearchConfig->fSearchPolicy );
				if ( siResult != eDSNoErr ) throw( siResult );
				//KW need to update this file
				//siResult = pConfigFromXML->SetSearchPolicy(inSearchPolicy);
				//if ( siResult != eDSNoErr ) throw( siResult );
			}

			//switch the search policy here
			inSearchConfig->fSearchPolicy = inSearchPolicy;
		
			//since switching need to remove the old and
			//need to cleanup the struct list ie. the internals
			CleanSearchListData( inSearchConfig->fSearchNodeList );
			inSearchConfig->fSearchNodeList = nil;

			switch ( inSearchConfig->fSearchPolicy )
			{
				case kCustomSearchPolicy:
					CShared::LogIt( 0x0F, "Setting search policy to Custom search" );
					inSearchConfig->fSearchNodeList = inSearchConfig->pConfigFromXML->GetCustom();
					//if custom list was nil we go ahead anyways with local only
					//local policy nodes always added in regardless
					siResult = AddLocalNodesAsFirstPaths(&(inSearchConfig->fSearchNodeList));
					break;

				case kLocalSearchPolicy:
					CShared::LogIt( 0x0F, "Setting search policy to Local search" );
					//local policy call
					siResult = AddLocalNodesAsFirstPaths(&(inSearchConfig->fSearchNodeList));
					break;

				case kNetInfoSearchPolicy:
				default:
					CShared::LogIt( 0x0F, "Setting search policy to NetInfo default" );
					siResult = DoNetInfoDefault(&(inSearchConfig->fSearchNodeList));
					break;
			} // select the search policy

			if (siResult == eDSNoErr)
			{
				if (inSearchConfig->fSearchPolicy == kNetInfoSearchPolicy)
				{
					//get the default LDAP search paths if they are present
					//don't check status on return as continuing on anyways
					//don't add on to the custom path
					if ( inSearchConfig->pConfigFromXML == nil
						 || inSearchConfig->pConfigFromXML->IsDHCPLDAPEnabled() )
					{
						addLDAPResult = AddDefaultLDAPNodesLast(&(inSearchConfig->fSearchNodeList));
					}
				}
				
				// make search node active
				fState = kUnknownState;
				fState += kInitalized;
				fState += kActive;
		
				//set the indicator file
				if (addLDAPResult == eSearchPathNotDefined)
				{
					SetSearchPolicyIndicatorFile( inSearchConfig->fSearchConfigKey, inSearchConfig->fSearchPolicy );
				}
				else //DHCP LDAP nodes added so make sure indicator file shows a custom policy
				{
					SetSearchPolicyIndicatorFile( inSearchConfig->fSearchConfigKey, kCustomSearchPolicy );
				}

				//let all the context node references know about the switch
				gSNNodeRef->DoOnAllItems(CSearchNode::ContextSetListChangedProc);
				
				CSearchNode::WakeUpRequests();
			}
			
	} // try

	catch( sInt32 err )
	{
		siResult = err;
		fState = kUnknownState;
		fState += kInactive;
	}
	
	fMutex.Signal();

	return( siResult );

} // SwitchSearchPolicy


// --------------------------------------------------------------------------------
//	* DoNetInfoDefault ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::DoNetInfoDefault ( sSearchList **inSearchNodeList )
{
	sInt32				siResult		= eDSNoErr;

	*inSearchNodeList = GetNetInfoPaths(false,nil);
	if ( *inSearchNodeList  == nil )
	{
		siResult	= eSearchPathNotDefined;
	}

	return( siResult );

} // DoNetInfoDefault


// --------------------------------------------------------------------------------
//	* GetNetInfoPaths ()
// --------------------------------------------------------------------------------

sSearchList *CSearchNode:: GetNetInfoPaths ( bool bFullPath, char** localNodeName )
{
	sInt32					siResult			= eDSNoErr;
	bool					bLocalIsRoot		= false;
	char				   *p					= nil;
	char				   *n					= nil;
	tDataBuffer			   *pNodeNameBuff 		= nil;
	tDataBuffer			   *pLocalNodeBuff 		= nil;
	tDataList			   *pNodeNameDL			= nil;
	tDataList			   *pNodePath			= nil;
	uInt32					uiCount				= 0;
	sSearchList			   *pCurList			= nil;
	sSearchList			   *pSrchList			= nil;
	uInt32					uiCntr				= 1;
	sSearchList			   *outSrchList			= nil;
	char				   *aSearchPath			= nil;
	tDirNodeReference  	   	aNodeRef			= 0;
	tAttributeListRef		attrListRef			= 0;
	tAttributeValueListRef	attrValueListRef	= 0;
	tAttributeValueEntry   *pAttrValueEntry		= nil;
	tAttributeEntry		   *pAttrEntry			= nil;
	uInt32					aIndex				= 0;
	uInt32					aSearchPathLen		= 0;
	bool					bSetLocalFirst		= true;
//	tDataList			   *aNodeName			= nil;
	tContextData			context				= NULL;


	try
	{
		if (localNodeName == nil || *localNodeName == nil)
		{
			pLocalNodeBuff = ::dsDataBufferAllocate( fDirRef, 512 );
			if ( pLocalNodeBuff == nil ) throw( (sInt32)eMemoryError );
	
			do 
			{
				siResult = dsFindDirNodes( fDirRef, pLocalNodeBuff, NULL, eDSLocalNodeNames, &uiCount, &context );
				if (siResult == eDSBufferTooSmall)
				{
					uInt32 bufSize = pLocalNodeBuff->fBufferSize;
					dsDataBufferDeallocatePriv( pLocalNodeBuff );
					pLocalNodeBuff = nil;
					pLocalNodeBuff = ::dsDataBufferAllocatePriv( bufSize * 2 );
				}
			} while (siResult == eDSBufferTooSmall);
		
			if ( siResult != eDSNoErr ) throw( siResult );
			if ( uiCount == 0 )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetNetInfoPaths:dsFindDirNodes on local returned zero" );
				throw( siResult ); //could end up throwing eDSNoErr but no local node will still return nil
			}
			
			// assume there is only one local node
			siResult = dsGetDirNodeName( fDirRef, pLocalNodeBuff, 1, &pNodeNameDL );
			if ( siResult != eDSNoErr )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetNetInfoPaths:dsGetDirNodeName on local returned error %d", siResult );
				throw( siResult );
			}
			
			if ( pLocalNodeBuff != nil )
			{
				::dsDataBufferDeAllocate( fDirRef, pLocalNodeBuff );
				pLocalNodeBuff = nil;
			}
			
			//open the local node
			siResult = ::dsOpenDirNode( fDirRef, pNodeNameDL, &aNodeRef );
			if ( siResult != eDSNoErr )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetNetInfoPaths:dsOpenDirNode on local returned error %d" );
				throw( siResult );
			}
	
			::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
			free(pNodeNameDL);
			pNodeNameDL = nil;
			
			pNodePath = ::dsBuildListFromStringsPriv( kDSNAttrNodePath, nil );
			if ( pNodePath == nil ) throw( (sInt32)eMemoryAllocError );
	
			pNodeNameBuff = ::dsDataBufferAllocate( fDirRef, 1024 );
			if ( pNodeNameBuff == nil ) throw( (sInt32)eMemoryError );
	
			//extract the "true" node path for the local node ie. not the registered label
			uiCount = 0;
			siResult = ::dsGetDirNodeInfo( aNodeRef, pNodePath, pNodeNameBuff, false, &uiCount, &attrListRef, nil  );
			if ( siResult != eDSNoErr ) throw( siResult );
			if ( uiCount == 0 ) throw ( (sInt32)eNoSearchNodesFound );
				
			::dsDataListDeAllocate( fDirRef, pNodePath, false );
			free(pNodePath);
			pNodePath = nil;
			
			//assume first attribute since only 1 asked for
			siResult = dsGetAttributeEntry( aNodeRef, pNodeNameBuff, attrListRef, 1, &attrValueListRef, &pAttrEntry );
			if ( siResult != eDSNoErr ) throw( siResult );
	
			//this node path here is multi-valued so we need to put it back together even
			//though we will parse it out below since we start at the bottom and not the top
			//in the search order
			//KW seems inefficient to go through the loop twice ie. first to get total length and second to build string
			//KW alternate approach is to put aSearchPath on the stack with size say of 256?
			//figure out the total path string length
			for (aIndex=1; aIndex < (pAttrEntry->fAttributeValueCount+1); aIndex++)
			{
				siResult = dsGetAttributeValue( aNodeRef, pNodeNameBuff, aIndex, attrValueListRef, &pAttrValueEntry );
				if ( siResult != eDSNoErr ) throw( siResult );
				if ( pAttrValueEntry->fAttributeValueData.fBufferData == nil ) throw( (sInt32)eMemoryAllocError );
				aSearchPathLen += (::strlen(pAttrValueEntry->fAttributeValueData.fBufferData) + 1); //+1 for the "/" to be added
				dsDeallocAttributeValueEntry(fDirRef, pAttrValueEntry);
				pAttrValueEntry = nil;
			}
			aSearchPath = (char *) ::calloc( 1, aSearchPathLen + 1);
			//build the actual path string
			for (aIndex=1; aIndex < (pAttrEntry->fAttributeValueCount+1); aIndex++)
			{
				siResult = dsGetAttributeValue( aNodeRef, pNodeNameBuff, aIndex, attrValueListRef, &pAttrValueEntry );
				if ( siResult != eDSNoErr ) throw( siResult );
				if ( pAttrValueEntry->fAttributeValueData.fBufferData == nil ) throw( (sInt32)eMemoryAllocError );
				::strcat(aSearchPath, "/");
				::strcat(aSearchPath, pAttrValueEntry->fAttributeValueData.fBufferData);
				dsDeallocAttributeValueEntry(fDirRef, pAttrValueEntry);
				pAttrValueEntry = nil;
			}
	
			dsCloseAttributeList(attrListRef);
			dsCloseAttributeValueList(attrValueListRef);
			dsDeallocAttributeEntry(fDirRef, pAttrEntry);
			pAttrEntry = nil;
	
			//close dir node after releasing attr references
			siResult = ::dsCloseDirNode(aNodeRef);
			if ( siResult != eDSNoErr ) throw( siResult );
			if (localNodeName != nil)
			{
				*localNodeName = strdup(aSearchPath);
			}
		}
		else if (localNodeName != nil)
		{
			aSearchPath = strdup(*localNodeName);
		}
		
   		if ( ::strcmp( "/NetInfo/root", aSearchPath ) == 0 )
   		{
   			bLocalIsRoot = true;
   		}

   		if ( ::strncmp( "/NetInfo/root", aSearchPath, 13 ) == 0 )
   		{
   			//p = (char *)::malloc( ::strlen( aSearchPath ) + 1 );
   			p = aSearchPath;
   			n = p + ::strlen( aSearchPath );

   			do
   			{
   				// Make the search list data node
				//would like to make use of method from CConfigs so that pSrchList = MakeListData(p);
				//replaces the next 17 lines but it needs the pointer to the CConfigs which would need to be passed
				//into this routine and is not readily available so do it the same way as the method
   				pSrchList = (sSearchList *)::calloc( sizeof( sSearchList ), sizeof(char) );

				if (bSetLocalFirst) //let's ensure that name is always fixed for the local node
				{
					if (bFullPath)
					{
						pSrchList->fNodeName = (char *)::calloc( ::strlen( p ) + 1, sizeof(char) );
						::strcpy( pSrchList->fNodeName, p );
					}
					else
					{
						pSrchList->fNodeName = (char *)::calloc( ::strlen( kstrDefaultLocalNodeName ) + 1, sizeof(char) );
						::strcpy( pSrchList->fNodeName, kstrDefaultLocalNodeName );
					}
				}
				else
				{
					pSrchList->fNodeName = (char *)::calloc( ::strlen( p ) + 1, sizeof(char) );
					::strcpy( pSrchList->fNodeName, p );
				}
				
				//KW this is a good point to check whether the fullpath pSrchList->fNodeName OR p name is registered
				//Two approaches:
				//A - simple - register every path found here since if already registered nothing new happens
				//B - call FindDirNodes to see if it already is registered - if not then register it
				// Looks like version A would be faster and NOT involve mach ip
				//aNodeName = ::dsBuildFromPathPriv( pSrchList->fNodeName, "/" );
				//if ( aNodeName != nil )
				//{
//doing this - if it succeeds will make the NetInfo registered node seem to be serviced
//by the Search plugin which is NOT what we want to have happen
//somehow this plugin needs to have the token from the NetInfo plugin or NOT do this
					//DSRegisterNode( fToken, aNodeName, kDirNodeType );

					//::dsDataListDeallocatePriv( aNodeName );
					//free( aNodeName );
					//aNodeName = nil;
				//}

				//ensure that local node is ALWAYS first in this path even if my_ni_pwdomain within
				//the NI plugin dsGetDirNodeInfo call returns a transitional path name
				//ie. don't care what pSrchList->fDataList name is used
				if (bSetLocalFirst)
				{
					pSrchList->fDataList = ::dsBuildFromPathPriv( kstrDefaultLocalNodeName, "/" );
					CShared::LogIt( 0x0F, "Search policy node %l = %s", uiCntr++, pSrchList->fNodeName );

					bSetLocalFirst  = false;

					//let's open lazily when we actually need the node ref
				}
				else
				{
					pSrchList->fDataList = ::dsBuildFromPathPriv( pSrchList->fNodeName, "/" );
					CShared::LogIt( 0x0F, "Search policy node %l = %s", uiCntr++, pSrchList->fNodeName );

					//let's open lazily when we actually need the node ref
				}

   				if ( outSrchList == nil )
   				{
   					outSrchList = pSrchList;
   					pCurList	= outSrchList;
   				}
   				else
   				{
   					// Add this search data node to the end of the list
   					pCurList->fNext = pSrchList;
   					pCurList = pCurList->fNext;
   				}

   				if ( ::strcmp( p, "/NetInfo/root" ) != 0 )
   				{
   					while ( (n != p) && (*n != '/') )
   					{
   						n--;
   					}

   					if ( *n == '/' )
   					{
   						//this is how we strip off the last component of the path
   						*n = '\0';
   					}
   				}
   				else
   				{
   					n = p;
   				}
   			} while ( (p != n) && (bLocalIsRoot == false) );

   			siResult = eDSNoErr;

   		} // if ( ::strncmp( "/NetInfo/root", aSearchPath, 13 ) == 0 )

		if ( p != nil )
   		{
			free( p ); //this also takes care of the aSearchPath so why have both?
   			aSearchPath = nil;
   			p = nil; // p is the same as aSearchPath
   		}

		if ( pNodeNameBuff != nil )
		{
			::dsDataBufferDeAllocate( fDirRef, pNodeNameBuff );
			pNodeNameBuff = nil;
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	if ( pNodeNameDL != nil )
	{
		::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
		free(pNodeNameDL);
		pNodeNameDL = nil;
	}

	if ( pNodePath != nil )
	{
		::dsDataListDeAllocate( fDirRef, pNodePath, false );
		free(pNodePath);
		pNodePath = nil;
	}

	if ( pLocalNodeBuff != nil )
	{
		::dsDataBufferDeAllocate( fDirRef, pLocalNodeBuff );
		pLocalNodeBuff = nil;
	}

	if ( pNodeNameBuff != nil )
	{
		::dsDataBufferDeAllocate( fDirRef, pNodeNameBuff );
		pNodeNameBuff = nil;
	}

	if ( aSearchPath != nil )
	{
		free( aSearchPath );
		aSearchPath = nil;
	}
	
	//ensure that there is a search node
	if (outSrchList == nil)
	{
		outSrchList = (sSearchList *)::calloc( sizeof( sSearchList ), sizeof(char) );
		if (outSrchList != nil)
		{
			outSrchList->fNodeName	= strdup(kstrDefaultLocalNodeName);
			outSrchList->fDataList	= ::dsBuildFromPathPriv( kstrDefaultLocalNodeName, "/" );
			outSrchList->fNodeRef	= 0;
			outSrchList->fOpened	= false;
			outSrchList->fNext		= nil;
			
			CShared::LogIt( 0x0F, "GetNetInfoPaths: Search policy node forced to explicit default local node due to failed init");
			//let's open lazily when we actually need the node ref
		}
		else
		{
			CShared::LogIt( 0x0F, "GetNetInfoPaths: Search policy node failed to force to explicit default local node due to failed init");
		}
	}

	return( outSrchList );

} // GetNetInfoPaths


// --------------------------------------------------------------------------------
//	* AddDefaultLDAPNodesLast ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::AddDefaultLDAPNodesLast( sSearchList **inSearchNodeList )
{
	sInt32				siResult		= eDSNoErr;
	sSearchList		   *ldapSrchList	= nil;
	sSearchList		   *pSrchList		= nil;

	ldapSrchList = GetDefaultLDAPPaths();
	if ( ldapSrchList == nil )
	{
		siResult = eSearchPathNotDefined;
	}
	else
	{
		if ( *inSearchNodeList == nil )
		{
			*inSearchNodeList = ldapSrchList;
		}
		else
		{
			// Add this search data list to the end
			pSrchList = *inSearchNodeList;
			while(pSrchList->fNext != nil)
			{
				pSrchList = pSrchList->fNext;
			}
			pSrchList->fNext = ldapSrchList;
		}
	}

	return( siResult );

} // AddDefaultLDAPNodesLast


// --------------------------------------------------------------------------------
//	* AddLocalNodesAsFirstPaths ()
// --------------------------------------------------------------------------------

sInt32 CSearchNode::AddLocalNodesAsFirstPaths( sSearchList **inSearchNodeList )
{
	sInt32				siResult		= eDSNoErr;
	sSearchList		   *localSrchList	= nil;
	sSearchList		   *pSrchList		= nil;

	char *localNodeName = nil;
	localNodeName = strdup( kstrDefaultLocalNodeName );
	localSrchList = GetLocalPaths(&localNodeName);
	free(localNodeName);
	if ( localSrchList  == nil )
	{
		siResult = eSearchPathNotDefined;
	}
	else
	{
		if ( *inSearchNodeList == nil )
		{
			*inSearchNodeList = localSrchList;
		}
		else
		{
			// Add this search data list to the start of the list
			pSrchList = localSrchList;
			while (pSrchList->fNext != nil)
			{
				pSrchList = pSrchList->fNext;
			}
			pSrchList->fNext = *inSearchNodeList;
			*inSearchNodeList = localSrchList;
		}
	}

	return( siResult );

} // AddLocalNodesAsFirstPaths


// --------------------------------------------------------------------------------
//	* GetLocalPaths ()
// --------------------------------------------------------------------------------

sSearchList *CSearchNode:: GetLocalPaths ( char** localNodeName )
{
	sInt32					siResult			= eDSNoErr;
	tDataBuffer			   *pNodeNameBuff 		= nil;
	tDataBuffer			   *pLocalNodeBuff 		= nil;
	tDataList			   *pNodeNameDL			= nil;
	tDataList			   *pNodePath			= nil;
	uInt32					uiCount				= 0;
//	uInt32					uiIndex				= 0;
	sSearchList			   *pCurList			= nil;
	sSearchList	 		   *pSrchList			= nil;
	uInt32					uiCntr				= 1;
	sSearchList			   *outSrchList			= nil;
	char				   *aSearchPath			= nil;
	tDirNodeReference  	   	aNodeRef			= 0;
	tAttributeListRef		attrListRef			= 0;
	tAttributeValueListRef	attrValueListRef	= 0;
	tAttributeValueEntry   *pAttrValueEntry		= nil;
	tAttributeEntry		   *pAttrEntry			= nil;
	uInt32					aIndex				= 0;
	uInt32					aSearchPathLen		= 0;
//	tDataList			   *aNodeName			= nil;
	tContextData			context				= NULL;


	try
	{
		if (localNodeName == nil || *localNodeName == nil)
		{
			pLocalNodeBuff = ::dsDataBufferAllocate( fDirRef, 512 );
			if ( pLocalNodeBuff == nil ) throw( (sInt32)eMemoryError );
	
			do 
			{
				siResult = dsFindDirNodes( fDirRef, pLocalNodeBuff, NULL, eDSLocalNodeNames, &uiCount, &context );
				if (siResult == eDSBufferTooSmall)
				{
					uInt32 bufSize = pLocalNodeBuff->fBufferSize;
					dsDataBufferDeallocatePriv( pLocalNodeBuff );
					pLocalNodeBuff = nil;
					pLocalNodeBuff = ::dsDataBufferAllocatePriv( bufSize * 2 );
				}
			} while (siResult == eDSBufferTooSmall);
		
			if ( siResult != eDSNoErr ) throw( siResult );
			if ( uiCount == 0 )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetLocalPaths:dsFindDirNodes on local returned zero" );
				throw( siResult ); //could end up throwing eDSNoErr but no local node will still return nil
			}
			
			// assume there is only one local node
			siResult = dsGetDirNodeName( fDirRef, pLocalNodeBuff, 1, &pNodeNameDL );
			if ( siResult != eDSNoErr )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetLocalPaths:dsGetDirNodeName on local returned error %d", siResult );
				throw( siResult );
			}

			if ( pLocalNodeBuff != nil )
			{
				::dsDataBufferDeAllocate( fDirRef, pLocalNodeBuff );
				pLocalNodeBuff = nil;
			}
			
			//open the local node
			siResult = ::dsOpenDirNode( fDirRef, pNodeNameDL, &aNodeRef );
			if ( siResult != eDSNoErr )
			{
				CShared::LogIt( 0x0F, "CSearchNode::GetLocalPaths:dsOpenDirNode on local returned error %d" );
				throw( siResult );
			}
	
			::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
			free(pNodeNameDL);
			pNodeNameDL = nil;
			
			pNodePath = ::dsBuildListFromStringsPriv( kDSNAttrNodePath, nil );
			if ( pNodePath == nil ) throw( (sInt32)eMemoryAllocError );
	
			pNodeNameBuff = ::dsDataBufferAllocate( fDirRef, 1024 );
			if ( pNodeNameBuff == nil ) throw( (sInt32)eMemoryError );
	
			//extract the "true" node path for the local node ie. not the registered label
			uiCount = 0;
			siResult = ::dsGetDirNodeInfo( aNodeRef, pNodePath, pNodeNameBuff, false, &uiCount, &attrListRef, nil  );
			if ( siResult != eDSNoErr ) throw( siResult );
			if ( uiCount == 0 ) throw ( (sInt32)eNoSearchNodesFound );
				
			::dsDataListDeAllocate( fDirRef, pNodePath, false );
			free(pNodePath);
			pNodePath = nil;
			
			//assume first attribute since only 1 asked for
			siResult = dsGetAttributeEntry( aNodeRef, pNodeNameBuff, attrListRef, 1, &attrValueListRef, &pAttrEntry );
			if ( siResult != eDSNoErr ) throw( siResult );
	
			//this node path here is multi-valued so we need to put it back together
			//KW seems inefficient to go through the loop twice ie. first to get total length and second to build string
			//KW alternate approach is to put aSearchPath on the stack with size say of 256?
			//figure out the total path string length
			for (aIndex=1; aIndex < (pAttrEntry->fAttributeValueCount+1); aIndex++)
			{
				siResult = dsGetAttributeValue( aNodeRef, pNodeNameBuff, aIndex, attrValueListRef, &pAttrValueEntry );
				if ( siResult != eDSNoErr ) throw( siResult );
				if ( pAttrValueEntry->fAttributeValueData.fBufferData == nil ) throw( (sInt32)eMemoryAllocError );
				aSearchPathLen += (::strlen(pAttrValueEntry->fAttributeValueData.fBufferData) + 1); //+1 for the "/" to be added
				dsDeallocAttributeValueEntry(fDirRef, pAttrValueEntry);
				pAttrValueEntry = nil;
			}
			aSearchPath = (char *) ::calloc( 1, aSearchPathLen + 1);
			//build the actual path string
			for (aIndex=1; aIndex < (pAttrEntry->fAttributeValueCount+1); aIndex++)
			{
				siResult = dsGetAttributeValue( aNodeRef, pNodeNameBuff, aIndex, attrValueListRef, &pAttrValueEntry );
				if ( siResult != eDSNoErr ) throw( siResult );
				if ( pAttrValueEntry->fAttributeValueData.fBufferData == nil ) throw( (sInt32)eMemoryAllocError );
				::strcat(aSearchPath, "/");
				::strcat(aSearchPath, pAttrValueEntry->fAttributeValueData.fBufferData);
				dsDeallocAttributeValueEntry(fDirRef, pAttrValueEntry);
				pAttrValueEntry = nil;
			}
	
			dsCloseAttributeList(attrListRef);
			dsCloseAttributeValueList(attrValueListRef);
			dsDeallocAttributeEntry(fDirRef, pAttrEntry);
			pAttrEntry = nil;
	
			//close dir node after releasing attr references
			siResult = ::dsCloseDirNode(aNodeRef);
			if ( siResult != eDSNoErr ) throw( siResult );
		}
		else if (localNodeName != nil && *localNodeName != nil)
		{
			aSearchPath = *localNodeName;
		}
   		if ( aSearchPath != nil )
   		{
			// Make the search list data node
			pSrchList = (sSearchList *)::calloc( sizeof( sSearchList ), sizeof(char) );

			pSrchList->fNodeName = strdup( aSearchPath );
			if (localNodeName != nil)
			{
				*localNodeName = aSearchPath;
			}
			else
			{
				free(aSearchPath);
			}
   			aSearchPath = nil;

			//KW RADAR 2703669 this is a good point to check whether the fullpath pSrchList->fNodeName is registered
			//Two approaches:
			//A - simple - register the path found here since if already registered nothing new happens
			//B - call FindDirNodes to see if it already is registered - if not then register it
			// Looks like version A would be faster and NOT involve mach ip
			//aNodeName = ::dsBuildFromPathPriv( pSrchList->fNodeName, "/" );
			//if ( aNodeName != nil )
			//{
//doing this - if it succeeds will make the NetInfo registered node seem to be serviced
//by the Search plugin which is NOT what we want to have happen
//somehow this plugin needs to have the token from the NetInfo plugin or NOT do this
				//DSRegisterNode( fToken, aNodeName, kDirNodeType );

				//::dsDataListDeallocatePriv( aNodeName );
				//free( aNodeName );
				//aNodeName = nil;
			//}

			pSrchList->fDataList = ::dsBuildFromPathPriv( kstrDefaultLocalNodeName, "/" );
			CShared::LogIt( 0x0F, "Search policy node %l = %s", uiCntr++, pSrchList->fNodeName );

			//let's open lazily when we actually need the node ref
			
			if ( outSrchList == nil )
			{
				outSrchList = pSrchList;
				outSrchList->fNext = nil;
				pCurList = outSrchList;
			}
			else
			{
				// Add this search data node to the end of the list
				pCurList->fNext = pSrchList;
				pCurList = pSrchList;
				pCurList->fNext = nil;
			}

   			siResult = eDSNoErr;

   		} // if ( aSearchPath != nil )

   		if ( pNodeNameDL != nil )
   		{
   			::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
			free(pNodeNameDL);
   			pNodeNameDL = nil;
   		}

		if ( pNodeNameBuff != nil )
		{
			::dsDataBufferDeAllocate( fDirRef, pNodeNameBuff );
			pNodeNameBuff = nil;
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	if ( pLocalNodeBuff != nil )
	{
		::dsDataBufferDeAllocate( fDirRef, pLocalNodeBuff );
		pLocalNodeBuff = nil;
	}

	if ( pNodeNameBuff != nil )
	{
		::dsDataBufferDeAllocate( fDirRef, pNodeNameBuff );
		pNodeNameBuff = nil;
	}

	if ( pNodePath != nil )
	{
		::dsDataListDeAllocate( fDirRef, pNodePath, false );
		free(pNodePath);
		pNodePath = nil;
	}

	if ( aSearchPath != nil )
	{
		free( aSearchPath );
		aSearchPath = nil;
	}

	//ensure that there is a search node
	if (outSrchList == nil)
	{
		outSrchList = (sSearchList *)::calloc( sizeof( sSearchList ), sizeof(char) );
		if (outSrchList != nil)
		{
			outSrchList->fNodeName	= strdup(kstrDefaultLocalNodeName);
			outSrchList->fDataList	= ::dsBuildFromPathPriv( kstrDefaultLocalNodeName, "/" );
			outSrchList->fNodeRef	= 0;
			outSrchList->fOpened	= false;
			outSrchList->fNext		= nil;
			
			CShared::LogIt( 0x0F, "GetLocalPaths: Search policy node forced to explicit default local node due to failed init");
			//let's open lazily when we actually need the node ref
		}
		else
		{
			CShared::LogIt( 0x0F, "GetLocalPaths: Search policy node failed to force to explicit default local node due to failed init");
		}
	}

	return( outSrchList );

} // GetLocalPaths


// --------------------------------------------------------------------------------
//	* GetDefaultLDAPPaths ()
// --------------------------------------------------------------------------------

sSearchList *CSearchNode:: GetDefaultLDAPPaths ( void )
{
	sInt32					siResult			= eDSNoErr;
	tDataBuffer			   *pNodeBuff 			= nil;
	tDataList			   *pNodeNameDL			= nil;
	tDataList			   *pNodeList			= nil;
	uInt32					uiCount				= 0;
	sSearchList			   *pCurList			= nil;
	sSearchList	 		   *pSrchList			= nil;
	uInt32					uiCntr				= 1;
	sSearchList			   *outSrchList			= nil;
	tDirNodeReference  	   	aNodeRef			= 0;
	tAttributeListRef		attrListRef			= 0;
	tAttributeValueListRef	attrValueListRef	= 0;
	tAttributeValueEntry   *pAttrValueEntry		= nil;
	tAttributeEntry		   *pAttrEntry			= nil;
	uInt32					aIndex				= 0;

//open the /LDAPv3 node and then call in to get the default LDAP server names
//use a call to dsGetDirNodeInfo

	try
	{
		pNodeBuff = ::dsDataBufferAllocate( fDirRef, 2048 );
		if ( pNodeBuff == nil ) throw( (sInt32)eMemoryError );
		
		pNodeNameDL = ::dsBuildListFromStringsPriv( "LDAPv3", nil );
		if ( pNodeNameDL == nil ) throw( (sInt32)eMemoryAllocError );

		//open the LDAPv3 node
		siResult = ::dsOpenDirNode( fDirRef, pNodeNameDL, &aNodeRef );
		if ( siResult != eDSNoErr ) throw( siResult );

		::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
		free(pNodeNameDL);
		pNodeNameDL = nil;
		
		pNodeList = ::dsBuildListFromStringsPriv( kDSNAttrDefaultLDAPPaths, nil );
		if ( pNodeList == nil ) throw( (sInt32)eMemoryAllocError );

		//extract the node list
		siResult = ::dsGetDirNodeInfo( aNodeRef, pNodeList, pNodeBuff, false, &uiCount, &attrListRef, nil  );
		if ( siResult != eDSNoErr ) throw( siResult );
		if ( uiCount == 0 ) throw ( (sInt32)eNoSearchNodesFound );
			
		::dsDataListDeAllocate( fDirRef, pNodeList, false );
		free(pNodeList);
		pNodeList = nil;

		//assume first attribute since only 1 expected
		siResult = dsGetAttributeEntry( aNodeRef, pNodeBuff, attrListRef, 1, &attrValueListRef, &pAttrEntry );
		if ( siResult != eDSNoErr ) throw( siResult );

		//retrieve the node path strings
		for (aIndex=1; aIndex < (pAttrEntry->fAttributeValueCount+1); aIndex++)
		{
			siResult = dsGetAttributeValue( aNodeRef, pNodeBuff, aIndex, attrValueListRef, &pAttrValueEntry );
			if ( siResult != eDSNoErr ) throw( siResult );
			if ( pAttrValueEntry->fAttributeValueData.fBufferData == nil ) throw( (sInt32)eMemoryAllocError );
			//pAttrValueEntry->fAttributeValueData.fBufferData
			//pAttrValueEntry->fAttributeValueData.fBufferLength
			
			// Make the search list data node
			pSrchList = (sSearchList *)::calloc( sizeof( sSearchList ), sizeof(char) );

			pSrchList->fNodeName = (char *)::calloc( pAttrValueEntry->fAttributeValueData.fBufferLength + 1, sizeof(char) );
			::strcpy( pSrchList->fNodeName, pAttrValueEntry->fAttributeValueData.fBufferData );

			pSrchList->fDataList = ::dsBuildFromPathPriv( pSrchList->fNodeName, "/" );
			CShared::LogIt( 0x0F, "Search policy node %l = %s", uiCntr++, pSrchList->fNodeName );

			if ( outSrchList == nil )
			{
				outSrchList = pSrchList;
				outSrchList->fNext = nil;
				pCurList = outSrchList;
			}
			else
			{
				// Add this LDAP v3 node to the end of the default LDAP v3 list
				pCurList->fNext = pSrchList;
				pCurList = pSrchList;
				pCurList->fNext = nil;
			}
			dsDeallocAttributeValueEntry(fDirRef, pAttrValueEntry);
			pAttrValueEntry = nil;
		}

		dsCloseAttributeList(attrListRef);
		dsCloseAttributeValueList(attrValueListRef);
		dsDeallocAttributeEntry(fDirRef, pAttrEntry);
		pAttrEntry = nil;

		//close dir node after releasing attr references
		siResult = ::dsCloseDirNode(aNodeRef);
		if ( siResult != eDSNoErr ) throw( siResult );

		if ( pNodeBuff != nil )
		{
			::dsDataBufferDeAllocate( fDirRef, pNodeBuff );
			pNodeBuff = nil;
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	if ( pNodeBuff != nil )
	{
		::dsDataBufferDeAllocate( fDirRef, pNodeBuff );
		pNodeBuff = nil;
	}

	if ( pNodeList != nil )
	{
		::dsDataListDeAllocate( fDirRef, pNodeList, false );
		free(pNodeList);
		pNodeList = nil;
	}

	if ( pNodeNameDL != nil )
	{
		::dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
		free(pNodeNameDL);
		pNodeNameDL = nil;
	}

	return( outSrchList );

} // GetDefaultLDAPPaths


//--------------------------------------------------------------------------------------------------
//	* WakeUpRequests() (static)
//
//--------------------------------------------------------------------------------------------------

void CSearchNode::WakeUpRequests ( void )
{
	gKickSearchRequests->Signal();
} // WakeUpRequests


// ---------------------------------------------------------------------------
//	* WaitForInit
//
// ---------------------------------------------------------------------------

void CSearchNode::WaitForInit ( void )
{
	volatile	uInt32		uiAttempts	= 0;

	while ( !(fState & kInitalized) &&
			!(fState & kFailedToInit) )
	{
		// Try for 2 minutes before giving up
		if ( uiAttempts++ >= 240 )
		{
			return;
		}

		// Now wait until we are told that there is work to do or
		//	we wake up on our own and we will look for ourselves

		gKickSearchRequests->Wait( (uInt32)(.5 * kMilliSecsPerSec) );

		gKickSearchRequests->Reset();
	}
} // WaitForInit


// ---------------------------------------------------------------------------
//	* ProcessRequest
//
// ---------------------------------------------------------------------------

sInt32 CSearchNode::ProcessRequest ( void *inData )
{
	sInt32		siResult	= eDSNoErr;
	char	   *pathStr		= nil;

	try
	{
		if ( inData == nil )
		{
			throw( (sInt32)ePlugInDataError );
		}

		if (((sHeader *)inData)->fType == kOpenDirNode)
		{
			if (((sOpenDirNode *)inData)->fInDirNodeName != nil)
			{
				pathStr = ::dsGetPathFromListPriv( ((sOpenDirNode *)inData)->fInDirNodeName, "/" );
				if ( (pathStr != nil) && (strncmp(pathStr,"/Search",7) != 0) )
				{
					throw( (sInt32)eDSOpenNodeFailed);
				}
			}
		}
		
		WaitForInit();

		if (fState & kFailedToInit) throw( (sInt32)ePlugInFailedToInitialize );
		if ( !(fState & kActive) ) throw( (sInt32)ePlugInNotActive );

		if ( ((sHeader *)inData)->fType == kHandleNetworkTransition )
		{
			HandleMultipleNetworkTransitions();
		}
		else if ( ((sHeader *)inData)->fType == kServerRunLoop )
		{
			if ( (((sHeader *)inData)->fContextData) != nil )
			{
				fServerRunLoop = (CFRunLoopRef)(((sHeader *)inData)->fContextData);
			}
		}
		else
		{
			siResult = HandleRequest( inData );
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}
	
	if (pathStr != nil)
	{
		free(pathStr);
		pathStr = nil;
	}

	return( siResult );

} // ProcessRequest


//------------------------------------------------------------------------------------
//	* HandleMultipleNetworkTransitions
//------------------------------------------------------------------------------------

void CSearchNode::HandleMultipleNetworkTransitions ( void )
{
	void	   *ptInfo		= nil;
	
	//let us be smart about doing the recheck
	//we would like to wait a short period for NetInfo to come back fully
	//we also don't want to re-init multiple times during this wait period
	//however we do go ahead and fire off timers each time
	//each call in here we update the delay time by 6 seconds
	//one more second than in NetInfo plugin
	fTransitionCheckTime = time(nil) + 6;

	if (fServerRunLoop != nil)
	{
		ptInfo = (void *)this;
		CFRunLoopTimerContext c = {0, (void*)ptInfo, NULL, NULL, NetworkChangeSNPICopyStringCallback};
	
		CFRunLoopTimerRef timer = CFRunLoopTimerCreate(	NULL,
														CFAbsoluteTimeGetCurrent() + 6,
														0,
														0,
														0,
														DoSNPINetworkChange,
														(CFRunLoopTimerContext*)&c);
	
		CFRunLoopAddTimer(fServerRunLoop, timer, kCFRunLoopDefaultMode);
		if (timer) CFRelease(timer);
	}
} // HandleMultipleNetworkTransitions


//------------------------------------------------------------------------------------
//	* ReDiscoverNetwork
//------------------------------------------------------------------------------------

void CSearchNode::ReDiscoverNetwork(void)
{
	//do something if the wait period has passed
	if (time(nil) >= fTransitionCheckTime)
	{
		Initialize(); // don't check return
	}
} // ReDiscoverNetwork


// ---------------------------------------------------------------------------
//	* HandleRequest
//
// ---------------------------------------------------------------------------

sInt32 CSearchNode::HandleRequest ( void *inData )
{
	sInt32		siResult	= eDSNoErr;
	sHeader	   *pMsgHdr		= nil;

	try
	{
		pMsgHdr = (sHeader *)inData;

		switch ( pMsgHdr->fType )
		{
			case kReleaseContinueData:
				siResult = ReleaseContinueData( (sReleaseContinueData *)inData );
				break;

			case kOpenDirNode:
				siResult = OpenDirNode( (sOpenDirNode *)inData );
				break;

			case kCloseDirNode:
				siResult = CloseDirNode( (sCloseDirNode *)inData );
				break;

			case kGetDirNodeInfo:
				siResult = GetDirNodeInfo( (sGetDirNodeInfo *)inData );
				break;

			case kGetRecordList:
				siResult = GetRecordList( (sGetRecordList *)inData );
				break;

			case kGetRecordEntry:
				siResult = GetRecordEntry( (sGetRecordEntry *)inData );
				break;

			case kGetAttributeEntry:
				siResult = GetAttributeEntry( (sGetAttributeEntry *)inData );
				break;

			case kGetAttributeValue:
				siResult = GetAttributeValue( (sGetAttributeValue *)inData );
				break;

			case kDoAttributeValueSearch:
			case kDoAttributeValueSearchWithData:
				siResult = AttributeValueSearch( (sDoAttrValueSearchWithData *)inData );
				break;

			case kCloseAttributeList:
				siResult = CloseAttributeList( (sCloseAttributeList *)inData );
				break;

			case kCloseAttributeValueList:
				siResult = CloseAttributeValueList( (sCloseAttributeValueList *)inData );
				break;

			case kDoPlugInCustomCall:
				siResult = DoPlugInCustomCall( (sDoPlugInCustomCall *)inData );
				break;
                                
			case kServerRunLoop:
				siResult = eDSNoErr;
				break;
				
			default:
				siResult = eNotHandledByThisNode;
				break;
		}

		pMsgHdr->fResult = siResult;
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	catch ( ... )
	{
		siResult = ePlugInError;
	}

	return( siResult );

} // HandleRequest


//------------------------------------------------------------------------------------
//	* ReleaseContinueData
//------------------------------------------------------------------------------------

sInt32 CSearchNode::ReleaseContinueData ( sReleaseContinueData *inData )
{
	sInt32	siResult	= eDSNoErr;

	// RemoveItem calls our ContinueDeallocProc to clean up
	if ( gSNContinue->RemoveItem( inData->fInContinueData ) != eDSNoErr )
	{
		siResult = eDSInvalidContext;
	}

	return( siResult );

} // ReleaseContinueData


//------------------------------------------------------------------------------------
//	* OpenDirNode
//------------------------------------------------------------------------------------

sInt32 CSearchNode::OpenDirNode ( sOpenDirNode *inData )
{
	sInt32				siResult			= eDSOpenNodeFailed;
	char			   *pathStr				= nil;
	sSearchContextData *pContext			= nil;
	sSearchConfig	   *aSearchConfigList	= nil;

	if ( inData != nil )
	{
		pathStr = ::dsGetPathFromListPriv( inData->fInDirNodeName, "/" );
		if ( pathStr != nil )
		{
			fMutex.Wait();
			aSearchConfigList = pSearchConfigList;
			while (aSearchConfigList != nil)
			{
				if ( ::strcmp( pathStr, aSearchConfigList->fSearchNodeName ) == 0 )
				{
					pContext = MakeContextData();
					if (pContext != nil)
					{
						//create a mutex for future use in a switch of search policy - only here in the node context
						pContext->pSearchListMutex = new DSMutexSemaphore();
						pContext->fSearchConfigKey = aSearchConfigList->fSearchConfigKey;
						//check if this is the default network node at which point we need to build the node list
						if (strcmp(pathStr, kstrNetworkNodeName) == 0)
						{
							pContext->fSearchNodeList = BuildNetworkNodeList();
						}
						else //regular type of search node ie. either auth or contacts
						{
							//get the search path list with new unique refs of each
							//search path node for use by this client who opened the search node
							pContext->fSearchNodeList = DupSearchListWithNewRefs(aSearchConfigList->fSearchNodeList);
						}
						if (aSearchConfigList->fSearchPolicy == kNetInfoSearchPolicy)
						{
							pContext->bAutoSearchList = true;
						}
						gSNNodeRef->AddItem( inData->fOutNodeRef, pContext );
						siResult = eDSNoErr;
					}
					break;
				}
				aSearchConfigList = aSearchConfigList->fNext;
			}
			fMutex.Signal();
			
			free( pathStr );
			pathStr = nil;
		}
	}

	return( siResult );

} // OpenDirNode


//------------------------------------------------------------------------------------
//	* CloseDirNode
//------------------------------------------------------------------------------------

sInt32 CSearchNode::CloseDirNode ( sCloseDirNode *inData )
{
	sInt32				siResult		= eDSNoErr;
	sSearchContextData *pContext		= nil;

	try
	{
		pContext = (sSearchContextData *) gSNNodeRef->GetItemData( inData->fInNodeRef );
		if ( pContext == nil ) throw( (sInt32)eDSInvalidNodeRef );

		gSNNodeRef->RemoveItem( inData->fInNodeRef );
		gSNContinue->RemoveItems( inData->fInNodeRef );
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	return( siResult );

} // CloseDirNode


//------------------------------------------------------------------------------------
//	* GetDirNodeInfo
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetDirNodeInfo ( sGetDirNodeInfo *inData )
{
	sInt32				siResult		= eDSNoErr;
	uInt32				uiOffset		= 0;
	uInt32				uiNodeCnt		= 0;
	char			   *p				= nil;
	char			   *localNodeName	= nil;
	uInt32				uiCntr			= 1;
	uInt32				uiAttrCnt		= 0;
	CAttributeList	   *inAttrList		= nil;
	char			   *pAttrName		= nil;
	char			   *pData			= nil;
	sSearchContextData *pAttrContext	= nil;
	sSearchList		   *pListPtr		= nil;
	sSearchList		   *pListPtrToo		= nil;
	sSearchList		   *pListCustom		= nil;
	CBuff				outBuff;
	char			   *policyValue		= nil;
	sSearchContextData *pContext		= nil;
	sSearchConfig	   *aSearchConfig	= nil;
	CDataBuff	 	   *aRecData		= nil;
	CDataBuff	 	   *aAttrData		= nil;
	CDataBuff	 	   *aTmpData		= nil;
	uInt32				searchNodeNameBufLen = 0;

	try
	{

		aRecData	= new CDataBuff();
		aAttrData	= new CDataBuff();
		aTmpData	= new CDataBuff();

		if ( inData  == nil ) throw( (sInt32)eMemoryError );

		pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInNodeRef );
		if ( pContext == nil ) throw( (sInt32)eDSInvalidNodeRef );
		
		fMutex.Wait();
		aSearchConfig	= FindSearchConfigWithKey(pContext->fSearchConfigKey);
		if ( aSearchConfig == nil ) throw( (sInt32)eDSInvalidNodeRef );		

		inAttrList = new CAttributeList( inData->fInDirNodeInfoTypeList );
		if ( inAttrList == nil ) throw( (sInt32)eDSNullNodeInfoTypeList );
		if (inAttrList->GetCount() == 0) throw( (sInt32)eDSEmptyNodeInfoTypeList );

		siResult = outBuff.Initialize( inData->fOutDataBuff, true );
		if ( siResult != eDSNoErr ) throw( siResult );

		// Set the real buffer type
		siResult = outBuff.SetBuffType( 'Gdni' ); //Cannot use 'StdA' since no tRecordEntry returned
		if ( siResult != eDSNoErr ) throw( siResult );

		aRecData->Clear();
		aAttrData->Clear();

		// Set the record name and type
		aRecData->AppendShort( ::strlen( "dsAttrTypeStandard:SearchNodeInfo" ) );
		aRecData->AppendString( "dsAttrTypeStandard:SearchNodeInfo" );
		if (aSearchConfig->fSearchNodeName != nil)
		{
			searchNodeNameBufLen = strlen( aSearchConfig->fSearchNodeName );
			aRecData->AppendShort( searchNodeNameBufLen );
			searchNodeNameBufLen += 2;
			aRecData->AppendString( aSearchConfig->fSearchNodeName );
		}
		else
		{
			aRecData->AppendShort( ::strlen( "SearchNodeInfo" ) );
			aRecData->AppendString( "SearchNodeInfo" );
			searchNodeNameBufLen = 16; //2 + 14 = 16
		}

		while ( inAttrList->GetAttribute( uiCntr++, &pAttrName ) == eDSNoErr )
		{
			if (	(::strcmp( pAttrName, kDSAttributesAll ) == 0)	|| 
				(::strcmp( pAttrName, kDS1AttrSearchPath ) == 0) )
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrSearchPath ) );
				aTmpData->AppendString( kDS1AttrSearchPath );

				if ( inData->fInAttrInfoOnly == false )
				{
					uiNodeCnt = 0;
					pListPtr = pContext->fSearchNodeList;
					while ( pListPtr != nil )
					{
						uiNodeCnt++;
						pListPtr = pListPtr->fNext;
					}

					// Attribute value count
					aTmpData->AppendShort( uiNodeCnt );

					pListPtr = pContext->fSearchNodeList;
					while ( pListPtr != nil )
					{
						p = pListPtr->fNodeName;

						// Append attribute value
						aTmpData->AppendLong( ::strlen( p ) );
						aTmpData->AppendString( p );

						pListPtr = pListPtr->fNext;
					}
				}

				// Add the attribute length
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}
			
			if ( (	(::strcmp( pAttrName, kDSAttributesAll ) == 0)	|| 
					(::strcmp( pAttrName, kDS1AttrNSPSearchPath ) == 0) ) &&
					(pContext->fSearchConfigKey != eDSNetworkSearchNodeName) )
				//NetInfo search policy path
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrNSPSearchPath ) );
				aTmpData->AppendString( kDS1AttrNSPSearchPath );

				if ( inData->fInAttrInfoOnly == false )
				{
					uiNodeCnt = 0;
					pListPtr = GetNetInfoPaths(true, &localNodeName);
					if ( pListPtr == nil ) throw( (sInt32)eSearchPathNotDefined );
					if ( aSearchConfig->pConfigFromXML == nil
						 || aSearchConfig->pConfigFromXML->IsDHCPLDAPEnabled() )
					{
						AddDefaultLDAPNodesLast(&pListPtr);
					}
					
					pListPtrToo = pListPtr;
					while ( pListPtr != nil )
					{
						uiNodeCnt++;
						pListPtr = pListPtr->fNext;
					}

					// Attribute value count
					aTmpData->AppendShort( uiNodeCnt );

					pListPtr = pListPtrToo;
					while ( pListPtr != nil )
					{
						p = pListPtr->fNodeName;

						// Append attribute value
						aTmpData->AppendLong( ::strlen( p ) );
						aTmpData->AppendString( p );

						pListPtr = pListPtr->fNext;
					}
					
					//need to cleanup the struct list ie. the internals
					CleanSearchListData( pListPtrToo );
					pListPtrToo = nil;
				}

				// Add the attribute length
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}
			
			if ( (	(::strcmp( pAttrName, kDSAttributesAll ) == 0)	|| 
					(::strcmp( pAttrName, kDS1AttrLSPSearchPath ) == 0) ) &&
					(pContext->fSearchConfigKey != eDSNetworkSearchNodeName) )
				//Local search policy path
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrLSPSearchPath ) );
				aTmpData->AppendString( kDS1AttrLSPSearchPath );

				if ( inData->fInAttrInfoOnly == false )
				{
					uiNodeCnt = 0;
					pListPtr = GetLocalPaths(&localNodeName);
					if ( pListPtr == nil ) throw( (sInt32)eSearchPathNotDefined );

					pListPtrToo = pListPtr;
					while ( pListPtr != nil )
					{
						uiNodeCnt++;
						pListPtr = pListPtr->fNext;
					}

					// Attribute value count
					aTmpData->AppendShort( uiNodeCnt );

					pListPtr = pListPtrToo;
					while ( pListPtr != nil )
					{
						p = pListPtr->fNodeName;

						// Append attribute value
						aTmpData->AppendLong( ::strlen( p ) );
						aTmpData->AppendString( p );

						pListPtr = pListPtr->fNext;
					}
					
					//need to cleanup the struct list ie. the internals
					CleanSearchListData( pListPtrToo );
					pListPtrToo = nil;
				}

				// Add the attribute length
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}
			
			if ( (	(::strcmp( pAttrName, kDSAttributesAll ) == 0)	|| 
					(::strcmp( pAttrName, kDS1AttrCSPSearchPath ) == 0) ) &&
					(pContext->fSearchConfigKey != eDSNetworkSearchNodeName) )
				//Custom search policy path
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrCSPSearchPath ) );
				aTmpData->AppendString( kDS1AttrCSPSearchPath );

				if ( inData->fInAttrInfoOnly == false )
				{
					//get the custom portion
					if ( aSearchConfig->pConfigFromXML != nil )
					{
						pListCustom = aSearchConfig->pConfigFromXML->GetCustom();
					}
					//get the local portion
					pListPtr = GetLocalPaths(&localNodeName);
					if ( pListPtr == nil ) throw( (sInt32)eSearchPathNotDefined );
					
					//add the local to the front of the custom
					pListPtrToo = pListPtr;
					while ( pListPtrToo->fNext != nil )
					{
						pListPtrToo = pListPtrToo->fNext;
					}
					pListPtrToo->fNext = pListCustom;

					uiNodeCnt = 0;
					pListPtrToo = pListPtr;
					while ( pListPtr != nil )
					{
						uiNodeCnt++;
						pListPtr = pListPtr->fNext;
					}

					// Attribute value count
					aTmpData->AppendShort( uiNodeCnt );

					pListPtr = pListPtrToo;
					while ( pListPtr != nil )
					{
						p = pListPtr->fNodeName;

						// Append attribute value
						aTmpData->AppendLong( ::strlen( p ) );
						aTmpData->AppendString( p );

						pListPtr = pListPtr->fNext;
					}
					
					//need to cleanup the struct list ie. the internals
					CleanSearchListData( pListPtrToo );
					pListPtrToo = nil;
				}

				// Add the attribute length
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}
			
			if (	(::strcmp( pAttrName, kDSAttributesAll ) == 0)	|| 
				(::strcmp( pAttrName, kDS1AttrSearchPolicy ) == 0) )
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrSearchPolicy ) );
				aTmpData->AppendString( kDS1AttrSearchPolicy );

				if ( inData->fInAttrInfoOnly == false )
				{
					// Attribute value count
					aTmpData->AppendShort( 1 );

					if (aSearchConfig->fSearchPolicy == kNetInfoSearchPolicy)
					{
						policyValue = new char[1+strlen(kDS1AttrNSPSearchPath)];
						strcpy(policyValue, kDS1AttrNSPSearchPath);
					}
					else if (aSearchConfig->fSearchPolicy == kLocalSearchPolicy)
					{
						policyValue = new char[1+strlen(kDS1AttrLSPSearchPath)];
						strcpy(policyValue, kDS1AttrLSPSearchPath);
					}
					else if (aSearchConfig->fSearchPolicy == kCustomSearchPolicy)
					{
						policyValue = new char[1+strlen(kDS1AttrCSPSearchPath)];
						strcpy(policyValue, kDS1AttrCSPSearchPath);
					}
					else
					{
						policyValue = new char[1+strlen("Unknown")];
						strcpy(policyValue,"Unknown");
					}
					
					// Append attribute value
					aTmpData->AppendLong( ::strlen( policyValue ) );
					aTmpData->AppendString( policyValue );
				}

				// Add the attribute length
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}

			if ( (::strcmp( pAttrName, kDSAttributesAll ) == 0) || 
				 (::strcmp( pAttrName, kDS1AttrReadOnlyNode ) == 0) )
			{
				aTmpData->Clear();

				uiAttrCnt++;

				// Append the attribute name
				aTmpData->AppendShort( ::strlen( kDS1AttrReadOnlyNode ) );
				aTmpData->AppendString( kDS1AttrReadOnlyNode );

				if ( inData->fInAttrInfoOnly == false )
				{
					// Attribute value count
					aTmpData->AppendShort( 1 );

					//possible for a node to be ReadOnly, ReadWrite, WriteOnly
					//note that ReadWrite does not imply fully readable or writable
					
					// Add the root node as an attribute value
					aTmpData->AppendLong( ::strlen( "ReadOnly" ) );
					aTmpData->AppendString( "ReadOnly" );

				}
				// Add the attribute length and data
				aAttrData->AppendLong( aTmpData->GetLength() );
				aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

				// Clear the temp block
				aTmpData->Clear();
			}
				 
//			else if ( ::strcmp( pAttrName, kDS1AttrCapabilities ) == 0 )
//			else if ( ::strcmp( pAttrName, kDSNAttrRecordType ) == 0 )
		} // while loop over the attributes requested

		fMutex.Signal();
		aRecData->AppendShort( uiAttrCnt );
		aRecData->AppendBlock( aAttrData->GetData(), aAttrData->GetLength() );

		outBuff.AddData( aRecData->GetData(), aRecData->GetLength() );
		inData->fOutAttrInfoCount = uiAttrCnt;

		pData = outBuff.GetDataBlock( 1, &uiOffset );
		if ( pData != nil )
		{
			pAttrContext = MakeContextData();
			
		//add to the offset for the attr list the length of the GetDirNodeInfo fixed record labels
//		record length = 4
//		aRecData->AppendShort( ::strlen( "dsAttrTypeStandard:SearchNodeInfo" ) ); = 2
//		aRecData->AppendString( "dsAttrTypeStandard:SearchNodeInfo" ); = 33
//		aRecData->AppendShort( ::strlen( "SearchNodeInfo" ) ); = see above for distinct node
//		aRecData->AppendString( "SearchNodeInfo" ); = see above for distinct node
//		total adjustment = 4 + 2 + 33 + 2 + 14 = 39

			pAttrContext->offset = uiOffset + 39 + searchNodeNameBufLen;

			gSNNodeRef->AddItem( inData->fOutAttrListRef, pAttrContext );
		}
		else
		{
			siResult = eDSBufferTooSmall;
		}
		
		inData->fOutDataBuff->fBufferLength = inData->fOutDataBuff->fBufferSize;
	}

	catch( sInt32 err )
	{
		siResult = err;
		fMutex.Signal();
	}

	if ( localNodeName != nil )
	{
		free( localNodeName );
		localNodeName = nil;
	}
	if ( inAttrList != nil )
	{
		delete( inAttrList );
		inAttrList = nil;
	}
	if (policyValue != nil)
	{
		delete( policyValue );
	}

	if ( aRecData != nil )
	{
		delete(aRecData);
		aRecData = nil;
	}
	
	if ( aAttrData != nil )
	{
		delete(aAttrData);
		aAttrData = nil;
	}
	
	if ( aTmpData != nil )
	{
		delete(aTmpData);
		aTmpData = nil;
	}

	return( siResult );

} // GetDirNodeInfo


//------------------------------------------------------------------------------------
//	* GetRecordList
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetRecordList ( sGetRecordList *inData )
{
	sInt32				siResult		= eDSNoErr;
	uInt32				recCount		= 0;
	bool				done			= false;
	sSearchContinueData	*pContinue		= nil;
	sSearchContinueData	*pInContinue	= nil;
	eSearchState		runState		= keGetRecordList;
	eSearchState		lastState		= keUnknownState;
	CBuff				inOutBuff;
	sSearchContextData *pContext		= nil;
	sSearchConfig	   *aSearchConfig	= nil;
	bool				bKeepOldBuffer	= false;

	try
	{
		pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInNodeRef );
		if ( pContext == nil ) throw( (sInt32)eDSInvalidNodeRef );

		if (pContext->pSearchListMutex == nil ) throw( (sInt32)eDSBadContextData);

		// it's important to always aquire the global mutex before the individual
		// reference mutex to avoid deadlock
		fMutex.Wait();
		pContext->pSearchListMutex->Wait();
					
		aSearchConfig	= FindSearchConfigWithKey(pContext->fSearchConfigKey);
		if ( aSearchConfig == nil ) throw( (sInt32)eDSInvalidNodeRef );		

		//switch search policy does not work with the DefaultNetwork Node
		//check whether search policy has switched and if it has then adjust to the new one
		if ( ( pContext->bListChanged ) && ( pContext->fSearchConfigKey != eDSNetworkSearchNodeName ) )
		{
			if ( inData->fIOContinueData != nil )
			{
				//in the middle of continue data the search policy has changed so exit here
				throw( (sInt32)eDSInvalidContinueData); //KW would like a more appropriate error code
			}
			else
			{
				//switch the search policy to the current one
				//flush the old search path list
				CleanSearchListData( pContext->fSearchNodeList );

				//remove all existing continue data off of this reference
				gSNContinue->RemoveItems( inData->fInNodeRef );
					
				//get the updated search path list with new unique refs of each
				//search path node for use by this client who opened the search node
				pContext->fSearchNodeList = DupSearchListWithNewRefs(aSearchConfig->fSearchNodeList);
					
				if (aSearchConfig->fSearchPolicy == kNetInfoSearchPolicy)
				{
					pContext->bAutoSearchList = true;
				}
				else
				{
					pContext->bAutoSearchList = false;
				}
				
				//reset the flag
				pContext->bListChanged	= false;
			}
		}
		fMutex.Signal();
		
		// Set it to the first node in the search list - this check doesn't need to use the context search path
		if ( pContext->fSearchNodeList == nil ) throw( (sInt32)eSearchPathNotDefined );

		if ( inData->fIOContinueData != nil )
		{
			if ( gSNContinue->VerifyItem( inData->fIOContinueData ) == true )
			{
				//KW why are we replacing this here??

				// Create the new
				pContinue = (sSearchContinueData *)::calloc( sizeof( sSearchContinueData ), sizeof( char ) );
				if ( pContinue == nil ) throw( (sInt32)eMemoryAllocError );

				pInContinue = (sSearchContinueData *)inData->fIOContinueData;

				pContinue->fDirRef			= pInContinue->fDirRef;
				pContinue->fNodeRef			= pInContinue->fNodeRef;
				pContinue->fAttrOnly		= pInContinue->fAttrOnly;
				pContinue->fRecCount		= pInContinue->fRecCount;
				pContinue->fRecIndex		= pInContinue->fRecIndex;
				pContinue->fMetaTypes		= pInContinue->fMetaTypes;
				pContinue->fState			= pInContinue->fState;
				pContinue->fAliasList		= pInContinue->fAliasList;
				pContinue->fAliasAttribute	= pInContinue->fAliasAttribute;
				
				//check to see if the buffer has been resized
				if (inData->fInDataBuff->fBufferSize != pInContinue->fDataBuff->fBufferSize)
				{
					//need to save the contents of the buffer if there is something still there that we need
					//ie. check for pContinue->fState == keAddDataToBuff
					if (pContinue->fState == keAddDataToBuff)
					{
						//can we stall on this new allocation until we extract the remaining blocks?
						bKeepOldBuffer = true;
						pContinue->fDataBuff	= pInContinue->fDataBuff; //save the old buffer
						pInContinue->fDataBuff	= nil; //clean up separately in this case
					}
					else
					{
						pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fInDataBuff->fBufferSize );
						if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );
					}
					//pInContinue->fDataBuff will get freed below in gSNContinue->RemoveItem
				}
				else
				{
					pContinue->fDataBuff	= pInContinue->fDataBuff;
					pInContinue->fDataBuff	= nil;
				}
				pContinue->fContextData		= pInContinue->fContextData;
				pContinue->fLimitRecSearch	= pInContinue->fLimitRecSearch;
				pContinue->fTotalRecCount	= pInContinue->fTotalRecCount;

				// RemoveItem calls our ContinueDeallocProc to clean up
				// since we transfered ownership of these pointers we need to make sure they
				// are nil so the ContinueDeallocProc doesn't free them now
				pInContinue->fAliasList			= nil;
				pInContinue->fAliasAttribute	= nil;
				pInContinue->fContextData		= nil;
				gSNContinue->RemoveItem( inData->fIOContinueData );

				pInContinue = nil;
				inData->fIOContinueData = nil;

				runState = pContinue->fState;
			}
			else
			{
				throw( (sInt32)eDSInvalidContinueData );
			}
		}
		else
		{
			pContinue = (sSearchContinueData *)::calloc( 1, sizeof( sSearchContinueData ) );
			if ( pContinue == nil ) throw( (sInt32)eMemoryAllocError );

			pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fInDataBuff->fBufferSize );
			if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );

			siResult = GetNextNodeRef( 0, &pContinue->fNodeRef, pContext );
			if ( siResult != eDSNoErr ) throw( siResult );

			pContinue->fDirRef = fDirRef;
			
			pContinue->fRecIndex		= 1;
			pContinue->fTotalRecCount	= 0;
			pContinue->fLimitRecSearch	= 0;
			//check if the client has requested a limit on the number of records to return
			//we only do this the first call into this context for pContinue
			if (inData->fOutRecEntryCount >= 0)
			{
				pContinue->fLimitRecSearch = inData->fOutRecEntryCount;
			}

			DoAliasCheck( inData->fInRecTypeList, inData->fInAttribTypeList, pContinue );
		}

		// Empty the out buffer
		siResult = inOutBuff.Initialize( inData->fInDataBuff, true );
		if ( siResult != eDSNoErr ) throw( siResult );

		siResult = inOutBuff.SetBuffType( 'StdA' );
		if ( siResult != eDSNoErr ) throw( siResult );

		inData->fIOContinueData		= nil;
		//need to return zero if no records found
		inData->fOutRecEntryCount	= 0;

		while ( !done )
		{
			// Do the task
			switch ( runState )
			{
				// Get the original record list request
				case keGetRecordList:
				{

					if (pContinue->fLimitRecSearch > pContinue->fTotalRecCount)
					{
						recCount = pContinue->fLimitRecSearch - pContinue->fTotalRecCount;
					}
					else
					{
						recCount = 0;
					}

					siResult = ::dsGetRecordList( pContinue->fNodeRef,
													pContinue->fDataBuff,
													inData->fInRecNameList,
													inData->fInPatternMatch,
													inData->fInRecTypeList,
													inData->fInAttribTypeList,
													inData->fInAttribInfoOnly,
													&recCount,
													&pContinue->fContextData );

					pContinue->fRecCount	= recCount;
					pContinue->fRecIndex	= 1;

					lastState = keGetRecordList;

				}
				break;

				// Add any data from the original record request to our own
				//	buffer format
				case keAddDataToBuff:
				{
					siResult = AddDataToOutBuff( pContinue, &inOutBuff, pContext );
					if (bKeepOldBuffer)
					{
						if (siResult == eDSNoErr)
						{
							if ( pContinue->fDataBuff != nil )
							{
								::dsDataBufferDeallocatePriv( pContinue->fDataBuff );
								pContinue->fDataBuff = nil;
							}
							pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fInDataBuff->fBufferSize );
							if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );
							bKeepOldBuffer = false;
						}
					}
					lastState = keAddDataToBuff;
				}
				break;
				
				// Get any alias records for this node
				case keGetAliases:
				{
					if (pContinue->fLimitRecSearch > pContinue->fTotalRecCount)
					{
						recCount = pContinue->fLimitRecSearch - pContinue->fTotalRecCount;
					}
					else
					{
						recCount = 0;
					}
					siResult = ::dsGetRecordList( pContinue->fNodeRef,
													pContinue->fDataBuff,
													inData->fInRecNameList, //KW matches for long name will never work this way
													inData->fInPatternMatch,
													pContinue->fAliasList,
													pContinue->fAliasAttribute,
													false, //inData->fInAttribInfoOnly,
													&recCount,
													&pContinue->fContextData );

					pContinue->fRecCount	= recCount;
					pContinue->fRecIndex	= 1;

					lastState = keGetAliases;
				}
				break;

				case keExpandAliases:
				{
					siResult = ExpandAliases( pContinue, &inOutBuff, inData, nil, pContext );

					lastState = keExpandAliases;
				}
				break;

				case keGetNextNodeRef:
				{
					siResult	= GetNextNodeRef( pContinue->fNodeRef, &pContinue->fNodeRef, pContext );
					lastState	= keGetNextNodeRef;
				}
				break;

				case keSetContinueData:
				{
					switch ( lastState )
					{
						case keAddDataToBuff:
						case keExpandAliases:
							inOutBuff.GetDataBlockCount( &inData->fOutRecEntryCount );
							//KW add to the total rec count what is going out for this call
							pContinue->fTotalRecCount += inData->fOutRecEntryCount;
							pContinue->fState = lastState;
							inData->fIOContinueData	= pContinue;
							gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							siResult = eDSNoErr;
							break;

						case keGetRecordList:
						case keGetNextNodeRef:
							inOutBuff.GetDataBlockCount( &inData->fOutRecEntryCount );
							//KW add to the total rec count what is going out for this call
							pContinue->fTotalRecCount += inData->fOutRecEntryCount;
							pContinue->fState = keGetRecordList;
							if ( siResult == kEndOfSearchNodeList )
							{
								siResult = eDSNoErr;
								inData->fIOContinueData = nil;
							}
							else
							{
								inData->fIOContinueData = pContinue;
								gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							}
							break;

						case keBufferTooSmall:
							if (pContinue->fContextData == nil) //buffer too small in search node itself
							{
								pContinue->fState = keAddDataToBuff;
							}
							else //buffer too small in a search path node
							{
								pContinue->fState = keGetRecordList;
							}
							inData->fIOContinueData	= pContinue;
							gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							siResult = eDSBufferTooSmall;
							break;

						default:
							CShared::LogIt( 0x0F, "*** Invalid continue state = %l", lastState );
							break;
					}
				}
				break;

				case keDone:
				{
					if ( pContinue != nil )
					{
						CSearchNode::ContinueDeallocProc( pContinue );
						pContinue = nil;
					}
					done = true;
				}
				break;

				default:
				{
					CShared::LogIt( 0x0F, "*** Unknown run state = %l", runState );
					done = true;
				}
				break;

			} // switch for run state


			// *** Change State ***

			switch ( runState )
			{
				case keGetRecordList:
				{
					// Did dsGetRecordList succeed 
					if ( siResult == eDSNoErr )
					{
						// Did we find any records
						if ( pContinue->fRecCount != 0 )
						{
							// We found records, add them to our out buff
							runState = keAddDataToBuff;
						}
						else if (pContinue->fContextData == nil)
						{
							// No records were found on this node, do
							//	we need to read aliases for this node
							if ( pContinue->fAliasList != nil )
							{
								runState = keGetAliases;
							}
							else 
							{
								runState = keGetNextNodeRef;
							}
						}
					}
					//condition on eDSRecordNotFound will no longer be needed
					else if ( (siResult == eDSRecordNotFound ) ||
						  (siResult == eDSInvalidRecordName) ||
						  (siResult == eDSInvalidRecordType) )
						  //move on to aliases or the next node if these
						  //conditions are met
					{
						// No records were found on this node, do
						//	we need to read aliases for this node
						if ( pContinue->fAliasList != nil )
						{
							runState = keGetAliases;
						}
						else
						{
							runState = keGetNextNodeRef;
						}
					}
					else if (siResult == eDSBufferTooSmall)
					{
						lastState	= keBufferTooSmall;
						runState	= keSetContinueData;
					}
					else //move on to the next node
					{
						runState = keGetNextNodeRef;
					}
				}
				break;

				case keAddDataToBuff:
				{
					uInt32 aRecCnt = 0;
					inOutBuff.GetDataBlockCount(&aRecCnt);
					// Did we add all records to our buffer 
					if ( ( siResult == eDSNoErr ) || ( ( siResult == CBuff::kBuffFull ) && (aRecCnt > 0) ) )
					{
						inData->fOutRecEntryCount = aRecCnt;
							
						//check if we retrieved all that was requested
						//continue data might even be nil here
						if ((pContinue->fLimitRecSearch <= (pContinue->fTotalRecCount + inData->fOutRecEntryCount)) &&
								(pContinue->fLimitRecSearch != 0))
						{
							//KW would seem that setting continue data when we know we are done is wrong
							//runState = keSetContinueData;
							
							//KW add to the total rec count what is at least going out for this call
							pContinue->fTotalRecCount += inData->fOutRecEntryCount;
							
							//KW don't know why we need this continue data anymore?
							pContinue->fState = runState;
							//gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							runState = keDone;
							inData->fIOContinueData	= nil;
							siResult = eDSNoErr;
						}
						else
						{
							if ( siResult == CBuff::kBuffFull )
							{
								runState = keSetContinueData;
							}
							// Do we need to continue the original read
							else if ( pContinue->fContextData )
							{
								lastState = keGetRecordList;
								//runState = keSetContinueData;
								runState = keGetRecordList;
							}
							else
							{
								// Do we need to read aliases for this node
								if ( pContinue->fAliasList != nil )
								{
									runState = keGetAliases;
								}
								else
								{
									runState = keGetNextNodeRef;
								}
							}
						}
					}
					else
					{
						if ( siResult == CBuff::kBuffFull )
						{
							runState = keSetContinueData;
							lastState = keBufferTooSmall;
						}
						else
						{
							// We got an error we don't know how to deal with, we be gone
							runState = keDone;
						}
					}
				}
				break;

				case keGetAliases:
				{
					// Did dsGetRecordList succeed 
					if ( siResult == eDSNoErr )
					{
						// We found records
						if ( pContinue->fRecCount != 0 )
						{
							runState = keExpandAliases;
						}
						else if (pContinue->fContextData == nil)
						{
							runState = keGetNextNodeRef;
						}
					}
					else //move on to the next node
					{
						runState = keGetNextNodeRef;
					}
				}
				break;

				case keExpandAliases:
				{
					if ( siResult == eDSNoErr )
					{
						// KW This needs to be analyzed and revisited
						if ( pContinue->fID > 5 )
						{
							if ( pContinue->fContextData )
							{
								// We are in a bad state and must exit
								pContinue->fContextData = nil;
								pContinue->fID = 0;
							}
						}
						else
						{
							inOutBuff.GetDataBlockCount( &inData->fOutRecEntryCount );
							
							//check if we retrieved all that was requested
							//continue data might even be nil here
							if ((pContinue->fLimitRecSearch <= (pContinue->fTotalRecCount + inData->fOutRecEntryCount)) &&
									(pContinue->fLimitRecSearch != 0))
							{
								//KW would seem that setting continue data when we know we are done is wrong
								//runState = keSetContinueData;
							
								//KW add to the total rec count what is at least going out for this call
								pContinue->fTotalRecCount += inData->fOutRecEntryCount;
							
								//KW don't know why we need this continue data anymore?
								pContinue->fState = runState;
								//gSNContinue->AddItem( pContinue, inData->fInNodeRef );
								runState = keDone;
								inData->fIOContinueData	= nil;
								siResult = eDSNoErr;
							}
							else
							{
								// Do we need to continue the original read
								if ( pContinue->fContextData )
								{
									runState = keGetAliases;
								}
								else
								{
									inOutBuff.GetDataBlockCount( &recCount );
									if ( recCount == 0 )
									{
										runState = keGetNextNodeRef;
									}
									else
									{
										// We have something to return
										runState = keSetContinueData;
									}
								}
							}
						}
					}
					else
					{
						if ( siResult == CBuff::kBuffFull )
						{
							runState = keSetContinueData;
						}
						else
						{
							// We got an error we don't know how to deal with, we be gone
							runState = keDone;
						}
					}
				}
				break;

				case keGetNextNodeRef:
				{
					inOutBuff.GetDataBlockCount( &recCount );
					if ( siResult == eDSNoErr )
					{
						if ( recCount == 0 )
						{
							runState = keGetRecordList;
						}
						else
						{
							runState = keSetContinueData;
						}
					}
					else
					{
						if ( siResult == kEndOfSearchNodeList )
						{
							runState = keSetContinueData;
						}
						else
						{
							runState = keDone;
						}
					}
				}
				break;

				case keSetContinueData:
				case keDone:
				case keError:
				{
					done = true;
				}
				break;

				default:
				{
					CShared::LogIt( 0x0F, "*** Unknown transition state = %l", runState );
					done = true;
				}
				break;

			} // switch for transition state
		}

		pContext->pSearchListMutex->Signal();
	
	}

	catch( sInt32 err )
	{
		fMutex.Signal();
		if (pContext->pSearchListMutex != nil)
		{
			pContext->pSearchListMutex->Signal();
		}
		siResult = err;
	}

	if ( (inData->fIOContinueData == nil) && (pContinue != nil ) )
	{
		// we have decided not to return contine data, need to free it
		CSearchNode::ContinueDeallocProc( pContinue );
		pContinue = nil;
	}

	return( siResult );

} // GetRecordList


//------------------------------------------------------------------------------------
//	* GetRecordEntry
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetRecordEntry ( sGetRecordEntry *inData )
{
	sInt32					siResult		= eDSNoErr;
	uInt32					uiIndex			= 0;
	uInt32					uiCount			= 0;
	uInt32					uiOffset		= 0;
	uInt32					uberOffset		= 0;
	char 				   *pData			= nil;
	tRecordEntryPtr			pRecEntry		= nil;
	sSearchContextData 	   *pContext		= nil;
	CBuff					inBuff;
	uInt32					offset			= 0;
	uInt16					usTypeLen		= 0;
	char				   *pRecType		= nil;
	uInt16					usNameLen		= 0;
	char				   *pRecName		= nil;
	uInt16					usAttrCnt		= 0;
	uInt32					buffLen			= 0;

	try
	{
		if ( inData  == nil ) throw( (sInt32)eMemoryError );
		if ( inData->fInOutDataBuff  == nil ) throw( (sInt32)eDSEmptyBuffer );
		if (inData->fInOutDataBuff->fBufferSize == 0) throw( (sInt32)eDSEmptyBuffer );

		siResult = inBuff.Initialize( inData->fInOutDataBuff );
		if ( siResult != eDSNoErr ) throw( siResult );

		siResult = inBuff.GetDataBlockCount( &uiCount );
		if ( siResult != eDSNoErr ) throw( siResult );

		uiIndex = inData->fInRecEntryIndex;
		if ((uiIndex > uiCount) || (uiIndex == 0)) throw( (sInt32)eDSInvalidIndex );

		pData = inBuff.GetDataBlock( uiIndex, &uberOffset );
		if ( pData  == nil ) throw( (sInt32)eDSCorruptBuffer );

		//assume that the length retrieved is valid
		buffLen = inBuff.GetDataBlockLength( uiIndex );
		
		// Skip past this same record length obtained from GetDataBlockLength
		pData	+= 4;
		offset	= 0; //buffLen does not include first four bytes

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the length for the record type
		::memcpy( &usTypeLen, pData, 2 );
		
		pData	+= 2;
		offset	+= 2;

		pRecType = pData;
		
		pData	+= usTypeLen;
		offset	+= usTypeLen;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the length for the record name
		::memcpy( &usNameLen, pData, 2 );
		
		pData	+= 2;
		offset	+= 2;

		pRecName = pData;
		
		pData	+= usNameLen;
		offset	+= usNameLen;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the attribute count
		::memcpy( &usAttrCnt, pData, 2 );

		pRecEntry = (tRecordEntry *)::calloc( 1, sizeof( tRecordEntry ) + usNameLen + usTypeLen + 4 + kBuffPad );

		pRecEntry->fRecordNameAndType.fBufferSize	= usNameLen + usTypeLen + 4 + kBuffPad;
		pRecEntry->fRecordNameAndType.fBufferLength	= usNameLen + usTypeLen + 4;

		// Add the record name length
		::memcpy( pRecEntry->fRecordNameAndType.fBufferData, &usNameLen, 2 );
		uiOffset += 2;

		// Add the record name
		::memcpy( pRecEntry->fRecordNameAndType.fBufferData + uiOffset, pRecName, usNameLen );
		uiOffset += usNameLen;

		// Add the record type length
		::memcpy( pRecEntry->fRecordNameAndType.fBufferData + uiOffset, &usTypeLen, 2 );

		// Add the record type
		uiOffset += 2;
		::memcpy( pRecEntry->fRecordNameAndType.fBufferData + uiOffset, pRecType, usTypeLen );

		pRecEntry->fRecordAttributeCount = usAttrCnt;

		pContext = MakeContextData();
		if ( pContext  == nil ) throw( (sInt32)eMemoryAllocError );

		pContext->offset = uberOffset + offset + 4;	// context used by next calls of GetAttributeEntry
													// include the four bytes of the buffLen
		
		gSNNodeRef->AddItem( inData->fOutAttrListRef, pContext );

		inData->fOutRecEntryPtr = pRecEntry;
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	return( siResult );

} // GetRecordEntry


//------------------------------------------------------------------------------------
//	* GetAttributeEntry
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetAttributeEntry ( sGetAttributeEntry *inData )
{
	sInt32					siResult			= eDSNoErr;
	uInt16					usAttrTypeLen		= 0;
	uInt16					usAttrCnt			= 0;
	uInt32					usAttrLen			= 0;
	uInt16					usValueCnt			= 0;
	uInt32					usValueLen			= 0;
	uInt32					i					= 0;
	uInt32					uiIndex				= 0;
	uInt32					uiAttrEntrySize		= 0;
	uInt32					uiOffset			= 0;
	uInt32					uiTotalValueSize	= 0;
	uInt32					offset				= 0;
	uInt32					buffSize			= 0;
	uInt32					buffLen				= 0;
	char				   *p			   		= nil;
	char				   *pAttrType	   		= nil;
	tDataBuffer			   *pDataBuff			= nil;
	tAttributeValueListRef	attrValueListRef	= 0;
	tAttributeEntryPtr		pAttribInfo			= nil;
	sSearchContextData 	   *pAttrContext		= nil;
	sSearchContextData 	   *pValueContext		= nil;

	try
	{
		if ( inData  == nil ) throw( (sInt32)eMemoryError );

		pAttrContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInAttrListRef );
		if ( pAttrContext  == nil ) throw( (sInt32)eDSBadContextData );

		uiIndex = inData->fInAttrInfoIndex;
		if (uiIndex == 0) throw( (sInt32)eDSInvalidIndex );
				
		pDataBuff = inData->fInOutDataBuff;
		if ( pDataBuff  == nil ) throw( (sInt32)eDSNullDataBuff );
		
		buffSize	= pDataBuff->fBufferSize;
		//buffLen		= pDataBuff->fBufferLength;
		//here we can't use fBufferLength for the buffLen SINCE the buffer is packed at the END of the data block
		//and the fBufferLength is the overall length of the data for all blocks at the end of the data block
		//the value ALSO includes the bookkeeping data at the start of the data block
		//so we need to read it here

		p		= pDataBuff->fBufferData + pAttrContext->offset;
		offset	= pAttrContext->offset;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffSize)  throw( (sInt32)eDSInvalidBuffFormat );
				
		// Get the attribute count
		::memcpy( &usAttrCnt, p, 2 );
		if (uiIndex > usAttrCnt) throw( (sInt32)eDSInvalidIndex );

		// Move 2 bytes
		p		+= 2;
		offset	+= 2;

		// Skip to the attribute that we want
		for ( i = 1; i < uiIndex; i++ )
		{
			// Do record check, verify that offset is not past end of buffer, etc.
			if (4 + offset > buffSize)  throw( (sInt32)eDSInvalidBuffFormat );
		
			// Get the length for the attribute
			::memcpy( &usAttrLen, p, 4 );

			// Move the offset past the length word and the length of the data
			p		+= 4 + usAttrLen;
			offset	+= 4 + usAttrLen;
		}

		// Get the attribute offset
		uiOffset = offset;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (4 + offset > buffSize)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the length for the attribute block
		::memcpy( &usAttrLen, p, 4 );

		// Skip past the attribute length
		p		+= 4;
		offset	+= 4;

		//set the bufLen to stricter range
		buffLen = offset + usAttrLen;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the length for the attribute type
		::memcpy( &usAttrTypeLen, p, 2 );
		
		pAttrType = p + 2;
		p		+= 2 + usAttrTypeLen;
		offset	+= 2 + usAttrTypeLen;
		
		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get number of values for this attribute
		::memcpy( &usValueCnt, p, 2 );
		
		p		+= 2;
		offset	+= 2;
		
		for ( i = 0; i < usValueCnt; i++ )
		{
			// Do record check, verify that offset is not past end of buffer, etc.
			if (4 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
			// Get the length for the value
			::memcpy( &usValueLen, p, 4 );
			
			p		+= 4 + usValueLen;
			offset	+= 4 + usValueLen;
			
			uiTotalValueSize += usValueLen;
		}

		uiAttrEntrySize = sizeof( tAttributeEntry ) + usAttrTypeLen + kBuffPad;
		pAttribInfo = (tAttributeEntry *)::calloc( 1, uiAttrEntrySize );

		pAttribInfo->fAttributeValueCount				= usValueCnt;
		pAttribInfo->fAttributeDataSize					= uiTotalValueSize;
		pAttribInfo->fAttributeValueMaxSize				= 512;				// KW this is not used anywhere
		pAttribInfo->fAttributeSignature.fBufferSize	= usAttrTypeLen + kBuffPad;
		pAttribInfo->fAttributeSignature.fBufferLength	= usAttrTypeLen;
		::memcpy( pAttribInfo->fAttributeSignature.fBufferData, pAttrType, usAttrTypeLen );

		attrValueListRef = inData->fOutAttrValueListRef;

		pValueContext = MakeContextData();
		if ( pValueContext  == nil ) throw( (sInt32)eMemoryAllocError );

		pValueContext->offset	= uiOffset;

		gSNNodeRef->AddItem( inData->fOutAttrValueListRef, pValueContext );

		inData->fOutAttrInfoPtr = pAttribInfo;
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	return( siResult );

} // GetAttributeEntry


//------------------------------------------------------------------------------------
//	* GetAttributeValue
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetAttributeValue ( sGetAttributeValue *inData )
{
	sInt32						siResult		= eDSNoErr;
	uInt16						usValueCnt		= 0;
	uInt32						usValueLen		= 0;
	uInt16						usAttrNameLen	= 0;
	uInt32						i				= 0;
	uInt32						uiIndex			= 0;
	uInt32						offset			= 0;
	char					   *p				= nil;
	tDataBuffer				   *pDataBuff		= nil;
	tAttributeValueEntry	   *pAttrValue		= nil;
	sSearchContextData 		   *pValueContext	= nil;
	uInt32						buffSize		= 0;
	uInt32						buffLen			= 0;
	uInt32						attrLen			= 0;

	try
	{
		pValueContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInAttrValueListRef );
		if ( pValueContext  == nil ) throw( (sInt32)eDSBadContextData );

		uiIndex = inData->fInAttrValueIndex;
		if (uiIndex == 0) throw( (sInt32)eDSInvalidIndex );
		
		pDataBuff = inData->fInOutDataBuff;
		if ( pDataBuff  == nil ) throw( (sInt32)eDSNullDataBuff );

		buffSize	= pDataBuff->fBufferSize;
		//buffLen		= pDataBuff->fBufferLength;
		//here we can't use fBufferLength for the buffLen SINCE the buffer is packed at the END of the data block
		//and the fBufferLength is the overall length of the data for all blocks at the end of the data block
		//the value ALSO includes the bookkeeping data at the start of the data block
		//so we need to read it here

		p		= pDataBuff->fBufferData + pValueContext->offset;
		offset	= pValueContext->offset;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (4 + offset > buffSize)  throw( (sInt32)eDSInvalidBuffFormat );
				
		// Get the buffer length
		::memcpy( &attrLen, p, 4 );

		//now add the offset to the attr length for the value of buffLen to be used to check for buffer overruns
		//AND add the length of the buffer length var as stored ie. 4 bytes
		buffLen		= attrLen + pValueContext->offset + 4;
		if (buffLen > buffSize)  throw( (sInt32)eDSInvalidBuffFormat );

		// Skip past the attribute length
		p		+= 4;
		offset	+= 4;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the attribute name length
		::memcpy( &usAttrNameLen, p, 2 );
		
		p		+= 2 + usAttrNameLen;
		offset	+= 2 + usAttrNameLen;

		// Do record check, verify that offset is not past end of buffer, etc.
		if (2 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		// Get the value count
		::memcpy( &usValueCnt, p, 2 );
		
		p		+= 2;
		offset	+= 2;

		if (uiIndex > usValueCnt)  throw( (sInt32)eDSInvalidIndex );

		// Skip to the value that we want
		for ( i = 1; i < uiIndex; i++ )
		{
			// Do record check, verify that offset is not past end of buffer, etc.
			if (4 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
			// Get the length for the value
			::memcpy( &usValueLen, p, 4 );
			
			p		+= 4 + usValueLen;
			offset	+= 4 + usValueLen;
		}

		// Do record check, verify that offset is not past end of buffer, etc.
		if (4 + offset > buffLen)  throw( (sInt32)eDSInvalidBuffFormat );
		
		::memcpy( &usValueLen, p, 4 );
		
		p		+= 4;
		offset	+= 4;

		//if (usValueLen == 0)  throw( (sInt32)eDSInvalidBuffFormat ); //if zero is it okay?

		pAttrValue = (tAttributeValueEntry *)::calloc( 1, sizeof( tAttributeValueEntry ) + usValueLen + kBuffPad );

		pAttrValue->fAttributeValueData.fBufferSize		= usValueLen + kBuffPad;
		pAttrValue->fAttributeValueData.fBufferLength	= usValueLen;
		
		// Do record check, verify that offset is not past end of buffer, etc.
		if ( usValueLen + offset > buffLen ) throw( (sInt32)eDSInvalidBuffFormat );
		
		::memcpy( pAttrValue->fAttributeValueData.fBufferData, p, usValueLen );

		// Set the attribute value ID
		pAttrValue->fAttributeValueID = 0x00;

		inData->fOutAttrValue = pAttrValue;
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	return( siResult );

} // GetAttributeValue
	

//------------------------------------------------------------------------------------
//	* AttributeValueSearch
//------------------------------------------------------------------------------------

sInt32 CSearchNode::AttributeValueSearch ( sDoAttrValueSearchWithData *inData )
{

	sInt32				siResult		= eDSNoErr;
	uInt32				recCount		= 0;
	bool				done			= false;
	sSearchContinueData	*pContinue		= nil;
	sSearchContinueData	*pInContinue	= nil;
	eSearchState		runState		= keGetRecordList;		//note that there is NO keAttributeValueSearch
																//but keGetRecordList is used here instead
	eSearchState		lastState		= keUnknownState;
	CBuff				inOutBuff;
	sSearchContextData *pContext		= nil;
	sSearchConfig	   *aSearchConfig	= nil;
	tDataList		   *allRecList		= nil;
	sInt32				allocResult		= eDSNoErr;
	bool				bKeepOldBuffer	= false;

	try
	{
		allRecList = (tDataList *) calloc( 1, sizeof( tDataList ) );
		allocResult = ::dsAppendStringToListPriv( allRecList, kDSRecordsAll );
		if ( allocResult != eDSNoErr ) throw( allocResult );
		
		pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInNodeRef );
		if ( pContext == nil ) throw( (sInt32)eDSInvalidNodeRef );
		
		if (pContext->pSearchListMutex == nil ) throw( (sInt32)eDSBadContextData);

		// it's important to always aquire the global mutex before the individual
		// reference mutex to avoid deadlock
		fMutex.Wait();
		pContext->pSearchListMutex->Wait();
					
		aSearchConfig	= FindSearchConfigWithKey(pContext->fSearchConfigKey);
		if ( aSearchConfig == nil ) throw( (sInt32)eDSInvalidNodeRef );		

		//switch search policy does not work with the DefaultNetwork Node
		//check whether search policy has switched and if it has then adjust to the new one
		if ( ( pContext->bListChanged ) && ( pContext->fSearchConfigKey != eDSNetworkSearchNodeName ) )
		{
			if ( inData->fIOContinueData != nil )
			{
				//in the middle of continue data the search policy has changed so exit here
				throw( (sInt32)eDSInvalidContinueData); //KW would like a more appropriate error code
			}
			else
			{
				//switch the search policy to the current one
				//flush the old search path list
				CleanSearchListData( pContext->fSearchNodeList );
					
				//remove all existing continue data off of this reference
				gSNContinue->RemoveItems( inData->fInNodeRef );
					
				//get the updated search path list with new unique refs of each
				//search path node for use by this client who opened the search node
				pContext->fSearchNodeList = DupSearchListWithNewRefs(aSearchConfig->fSearchNodeList);
					
				if (aSearchConfig->fSearchPolicy == kNetInfoSearchPolicy)
				{
					pContext->bAutoSearchList = true;
				}
				else
				{
					pContext->bAutoSearchList = false;
				}
				
				//reset the flag
				pContext->bListChanged	= false;
			}
		}
		fMutex.Signal();
		
		// Set it to the first node in the search list - this check doesn't need to use the context search path
		if ( pContext->fSearchNodeList == nil ) throw( (sInt32)eSearchPathNotDefined );

		if ( inData->fIOContinueData != nil )
		{
			if ( gSNContinue->VerifyItem( inData->fIOContinueData ) == true )
			{
				//KW why are we replacing this here??

				// Create the new
				pContinue = (sSearchContinueData *)::calloc( sizeof( sSearchContinueData ), sizeof(char) );
				if ( pContinue == nil ) throw( (sInt32)eMemoryAllocError );

				pInContinue = (sSearchContinueData *)inData->fIOContinueData;

				pContinue->fDirRef			= pInContinue->fDirRef;
				pContinue->fNodeRef			= pInContinue->fNodeRef;
				pContinue->fAttrOnly		= pInContinue->fAttrOnly;
				pContinue->fRecCount		= pInContinue->fRecCount;
				pContinue->fRecIndex		= pInContinue->fRecIndex;
				pContinue->fMetaTypes		= pInContinue->fMetaTypes;
				pContinue->fState			= pInContinue->fState;
				pContinue->fAliasList		= pInContinue->fAliasList;
				pContinue->fAliasAttribute	= pInContinue->fAliasAttribute;

				//check to see if the buffer has been resized
				if (inData->fOutDataBuff->fBufferSize != pInContinue->fDataBuff->fBufferSize)
				{
					//need to save the contents of the buffer if there is something still there that we need
					//ie. check for pContinue->fState == keAddDataToBuff
					if (pContinue->fState == keAddDataToBuff)
					{
						//can we stall on this new allocation until we extract the remaining blocks?
						bKeepOldBuffer = true;
						pContinue->fDataBuff	= pInContinue->fDataBuff; //save the old buffer
						pInContinue->fDataBuff	= nil; //clean up separately in this case
					}
					else
					{
						pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fOutDataBuff->fBufferSize );
						if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );
					}
					//pInContinue->fDataBuff will get freed below in gSNContinue->RemoveItem
				}
				else
				{
					pContinue->fDataBuff	= pInContinue->fDataBuff;
					pInContinue->fDataBuff	= nil;
				}
				pContinue->fContextData		= pInContinue->fContextData;
				pContinue->fLimitRecSearch	= pInContinue->fLimitRecSearch;
				pContinue->fTotalRecCount	= pInContinue->fTotalRecCount;

				// RemoveItem calls our ContinueDeallocProc to clean up
				// since we transfered ownership of these pointers we need to make sure they
				// are nil so the ContinueDeallocProc doesn't free them now
				pInContinue->fAliasList			= nil;
				pInContinue->fAliasAttribute	= nil;
				pInContinue->fContextData		= nil;
				gSNContinue->RemoveItem( inData->fIOContinueData );

				pInContinue = nil;
				inData->fIOContinueData = nil;

				runState = pContinue->fState;
			}
			else
			{
				throw( (sInt32)eDSInvalidContinueData );
			}
		}
		else
		{
			pContinue = (sSearchContinueData *)::calloc( 1, sizeof( sSearchContinueData ) );
			if ( pContinue == nil ) throw( (sInt32)eMemoryAllocError );

			pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fOutDataBuff->fBufferSize );
			if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );

			siResult = GetNextNodeRef( 0, &pContinue->fNodeRef, pContext );
			if ( siResult != eDSNoErr ) throw( siResult );
			
			pContinue->fDirRef = fDirRef;
			
			pContinue->fRecIndex		= 1;
			pContinue->fTotalRecCount	= 0;
			pContinue->fLimitRecSearch	= 0;
			//check if the client has requested a limit on the number of records to return
			//we only do this the first call into this context for pContinue
			if (inData->fOutMatchRecordCount >= 0)
			{
				pContinue->fLimitRecSearch = inData->fOutMatchRecordCount;
			}

			tDataList	myList;
			::memset( &myList, 0, sizeof( tDataList ) );

			allocResult = ::dsAppendStringToListPriv( &myList, inData->fInAttrType->fBufferData );
			if ( allocResult == eDSNoErr )
			{
				DoAliasCheck( inData->fInRecTypeList, &myList, pContinue );

				::dsDataListDeallocatePriv( &myList );
			}

		}

		// Empty the out buffer
		siResult = inOutBuff.Initialize( inData->fOutDataBuff, true );
		if ( siResult != eDSNoErr ) throw( siResult );

		siResult = inOutBuff.SetBuffType( 'StdA' );
		if ( siResult != eDSNoErr ) throw( siResult );

		inData->fIOContinueData			= nil;
		//need to return zero if no records found
		inData->fOutMatchRecordCount	= 0;
		

		while ( !done )
		{
			// Do the task
			switch ( runState )
			{
				// Get the original record list request
				case keGetRecordList:
				{
					if (pContinue->fLimitRecSearch > pContinue->fTotalRecCount)
					{
						recCount = pContinue->fLimitRecSearch - pContinue->fTotalRecCount;
					}
					else
					{
						recCount = 0;
					}
						if ( inData->fType == kDoAttributeValueSearchWithData )
						{
							siResult = ::dsDoAttributeValueSearchWithData(
																	pContinue->fNodeRef,
																	pContinue->fDataBuff,
																	inData->fInRecTypeList,
																	inData->fInAttrType,
																	inData->fInPattMatchType,
																	inData->fInPatt2Match,
																	inData->fInAttrTypeRequestList,
																	inData->fInAttrInfoOnly,
																	&recCount,
																	&pContinue->fContextData );
						}
						else
						{
							siResult = ::dsDoAttributeValueSearch(	pContinue->fNodeRef,
																	pContinue->fDataBuff,
																	inData->fInRecTypeList,
																	inData->fInAttrType,
																	inData->fInPattMatchType,
																	inData->fInPatt2Match,
																	&recCount,
																	&pContinue->fContextData );
						}

					pContinue->fRecCount	= recCount;
					pContinue->fRecIndex	= 1;

					lastState = keGetRecordList;
				}
				break;

				// Add any data from the original record request to our own
				//	buffer format
				case keAddDataToBuff:
				{
					siResult = AddDataToOutBuff( pContinue, &inOutBuff, pContext );
					if (bKeepOldBuffer)
					{
						if (siResult == eDSNoErr)
						{
							if ( pContinue->fDataBuff != nil )
							{
								::dsDataBufferDeallocatePriv( pContinue->fDataBuff );
								pContinue->fDataBuff = nil;
							}
							pContinue->fDataBuff = ::dsDataBufferAllocatePriv( inData->fOutDataBuff->fBufferSize );
							if ( pContinue->fDataBuff == nil ) throw( (sInt32)eMemoryAllocError );
							bKeepOldBuffer = false;
						}
					}
					lastState = keAddDataToBuff;
				}
				break;
				
				// Get any alias records for this node
				case keGetAliases:
				{
					if (pContinue->fLimitRecSearch > pContinue->fTotalRecCount)
					{
						recCount = pContinue->fLimitRecSearch - pContinue->fTotalRecCount;
					}
					else
					{
						recCount = 0;
					}
//Need to make this call to dsGetRecordList instead with the fAliasList arguments as above in GetRecordList method
//and kDSAllRecords and false flag to ensure we expand ALL the alias records on this node
//then can use the dsDoAttributeValueSearch(WithData) call WITHIN ExpandAliases method below as already written.

					siResult = ::dsGetRecordList(	pContinue->fNodeRef,
													pContinue->fDataBuff,
													allRecList,
													inData->fInPattMatchType,
													pContinue->fAliasList,
													pContinue->fAliasAttribute,
													false,
													&recCount,
													&pContinue->fContextData );

					pContinue->fRecCount	= recCount;
					pContinue->fRecIndex	= 1;

					lastState = keGetAliases;
				}
				break;

				case keExpandAliases:
				{
					siResult = ExpandAliases( pContinue, &inOutBuff, nil, inData, pContext );

					lastState = keExpandAliases;
				}
				break;

				case keGetNextNodeRef:
				{
					siResult	= GetNextNodeRef( pContinue->fNodeRef, &pContinue->fNodeRef, pContext );
					lastState	= keGetNextNodeRef;
				}
				break;

				case keSetContinueData:
				{
					switch ( lastState )
					{
						case keAddDataToBuff:
						case keExpandAliases:
							inOutBuff.GetDataBlockCount( &inData->fOutMatchRecordCount );
							//KW add to the total rec count what is going out for this call
							pContinue->fTotalRecCount += inData->fOutMatchRecordCount;
							pContinue->fState = lastState;
							inData->fIOContinueData	= pContinue;
							gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							siResult = eDSNoErr;
							break;

						case keGetRecordList:
						case keGetNextNodeRef:
							inOutBuff.GetDataBlockCount( &inData->fOutMatchRecordCount );
							//KW add to the total rec count what is going out for this call
							pContinue->fTotalRecCount += inData->fOutMatchRecordCount;
							pContinue->fState = keGetRecordList;
							if ( siResult == kEndOfSearchNodeList )
							{
								siResult = eDSNoErr;
								inData->fIOContinueData = nil;
							}
							else
							{
								inData->fIOContinueData = pContinue;
								gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							}
							break;

						case keBufferTooSmall:
							if (pContinue->fContextData == nil) //buffer too small in search node itself
							{
								pContinue->fState = keAddDataToBuff;
							}
							else //buffer too small in a search path node
							{
								pContinue->fState = keGetRecordList;
							}
							inData->fIOContinueData	= pContinue;
							gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							siResult = eDSBufferTooSmall;
							break;

						default:
							CShared::LogIt( 0x0F, "*** Invalid continue state = %l", lastState );
							break;
					}
				}
				break;

				case keDone:
				{
					if ( pContinue != nil )
					{
						CSearchNode::ContinueDeallocProc( pContinue );
						pContinue = nil;
					}
					done = true;
				}
				break;

				default:
				{
					CShared::LogIt( 0x0F, "*** Unknown run state = %l", runState );
					done = true;
				}
				break;

			} // switch for run state



			// *** Change State ***

			switch ( runState )
			{
				case keGetRecordList:
				{
					// Did dsGetRecordList succeed 
					if ( siResult == eDSNoErr )
					{
						// Did we find any records
						if ( pContinue->fRecCount != 0 )
						{
							// We found records, add them to our out buff
							runState = keAddDataToBuff;
						}
						else if (pContinue->fContextData == nil)
						{
							// No records were found on this node, do
							//	we need to read aliases for this node
							if ( pContinue->fAliasList != nil )
							{
								runState = keGetAliases;
							}
							else
							{
								runState = keGetNextNodeRef;
							}
						}
					}
					//condition on eDSRecordNotFound will no longer be needed
					else if ( (siResult == eDSRecordNotFound ) ||
						  (siResult == eDSInvalidRecordName) ||
						  (siResult == eDSInvalidRecordType) )
						  //move on to aliases or the next node if these
						  //conditions are met
					{
						// No records were found on this node, do
						//	we need to read aliases for this node
						if ( pContinue->fAliasList != nil )
						{
							runState = keGetAliases;
						}
						else
						{
							runState = keGetNextNodeRef;
						}
					}
					else if (siResult == eDSBufferTooSmall)
					{
						lastState	= keBufferTooSmall;
						runState	= keSetContinueData;
					}
					else //move on to the next node
					{
						runState = keGetNextNodeRef;
					}
				}
				break;

				case keAddDataToBuff:
				{
					uInt32 aRecCnt = 0;
					inOutBuff.GetDataBlockCount(&aRecCnt);
					// Did we add all records to our buffer 
					if ( ( siResult == eDSNoErr ) || ( ( siResult == CBuff::kBuffFull ) && (aRecCnt > 0) ) )
					{
						inData->fOutMatchRecordCount = aRecCnt;
							
						//check if we retrieved all that was requested
						//continue data might even be nil here
						if ((pContinue->fLimitRecSearch <= (pContinue->fTotalRecCount + inData->fOutMatchRecordCount)) &&
								(pContinue->fLimitRecSearch != 0))
						{
							//KW would seem that setting continue data when we know we are done is wrong
							//runState = keSetContinueData;
							
							//KW add to the total rec count what is at least going out for this call
							pContinue->fTotalRecCount += inData->fOutMatchRecordCount;
							
							//KW don't know why we need this continue data anymore?
							pContinue->fState = runState;
							//gSNContinue->AddItem( pContinue, inData->fInNodeRef );
							runState = keDone;
							inData->fIOContinueData	= nil;
							siResult = eDSNoErr;
						}
						else
						{
							if ( siResult == CBuff::kBuffFull )
							{
								runState = keSetContinueData;
							}
							// Do we need to continue the original read
							else if ( pContinue->fContextData )
							{
								lastState = keGetRecordList;
								//runState = keSetContinueData;
								runState = keGetRecordList;
							}
							else
							{
								// Do we need to read aliases for this node
								if ( pContinue->fAliasList != nil )
								{
									runState = keGetAliases;
								}
								else
								{
									runState = keGetNextNodeRef;
								}
							}
						}
					}
					else
					{
						if ( siResult == CBuff::kBuffFull )
						{
							runState = keSetContinueData;
							lastState = keBufferTooSmall;
						}
						else
						{
							// We got an error we don't know how to deal with, we be gone
							runState = keDone;
						}
					}
				}
				break;

				case keGetAliases:
				{
					if ( siResult == eDSNoErr )
					{
						// We found records
						if ( pContinue->fRecCount != 0 )
						{
							runState = keExpandAliases;
						}
						else if (pContinue->fContextData == nil)
						{
							runState = keGetNextNodeRef;
						}
					}
					else //move on to the next node
					{
						runState = keGetNextNodeRef;
					}
				}
				break;

				case keExpandAliases:
				{
					if ( siResult == eDSNoErr )
					{
						// KW This needs to be analyzed and revisited
						if ( pContinue->fID > 5 )
						{
							if ( pContinue->fContextData )
							{
								// We are in a bad state and must exit
								pContinue->fContextData = nil;
								pContinue->fID = 0;
							}
						}
						else
						{
							inOutBuff.GetDataBlockCount( &inData->fOutMatchRecordCount );
							
							//check if we retrieved all that was requested
							//continue data might even be nil here
							if ((pContinue->fLimitRecSearch <= (pContinue->fTotalRecCount + inData->fOutMatchRecordCount)) &&
									(pContinue->fLimitRecSearch != 0))
							{
								//KW would seem that setting continue data when we know we are done is wrong
								//runState = keSetContinueData;
							
								//KW add to the total rec count what is at least going out for this call
								pContinue->fTotalRecCount += inData->fOutMatchRecordCount;
							
								//KW don't know why we need this continue data anymore?
								pContinue->fState = runState;
								//gSNContinue->AddItem( pContinue, inData->fInNodeRef );
								runState = keDone;
								inData->fIOContinueData	= nil;
								siResult = eDSNoErr;
							}
							else
							{
								// Do we need to continue the original read
								if ( pContinue->fContextData )
								{
									runState = keGetAliases;
								}
								else
								{
									inOutBuff.GetDataBlockCount( &recCount );
									if ( recCount == 0 )
									{
										runState = keGetNextNodeRef;
									}
									else
									{
										// We have something to return
										runState = keSetContinueData;
									}
								}
							}
						}
					}
					else
					{
						if ( siResult == CBuff::kBuffFull )
						{
							runState = keSetContinueData;
						}
						else
						{
							// We got an error we don't know how to deal with, we be gone
							runState = keDone;
						}
					}
				}
				break;

				case keGetNextNodeRef:
				{
					inOutBuff.GetDataBlockCount( &recCount );
					if ( siResult == eDSNoErr )
					{
						if ( recCount == 0 )
						{
							runState = keGetRecordList;
						}
						else
						{
							runState = keSetContinueData;
						}
					}
					else
					{
						if ( siResult == kEndOfSearchNodeList )
						{
							runState = keSetContinueData;
						}
						else
						{
							runState = keDone;
						}
					}
				}
				break;

				case keSetContinueData:
				case keDone:
				case keError:
				{
					done = true;
				}
				break;

				default:
				{
					CShared::LogIt( 0x0F, "*** Unknown transition state = %l", runState );
					done = true;
				}
				break;

			} // switch for transition state
		}
		
		pContext->pSearchListMutex->Signal();
		
	}

	catch( sInt32 err )
	{
		fMutex.Signal();
		if (pContext->pSearchListMutex != nil)
		{
			pContext->pSearchListMutex->Signal();
		}
		siResult = err;
	}

	if ( (inData->fIOContinueData == nil) && (pContinue != nil ) )
	{
		// we have decided not to return contine data, need to free it
		CSearchNode::ContinueDeallocProc( pContinue );
		pContinue = nil;
	}

	if (allRecList != nil)
	{
		::dsDataListDeallocatePriv( allRecList );
		free(allRecList);
		allRecList = nil;
	}

	return( siResult );

} // AttributeValueSearch


//------------------------------------------------------------------------------------
//	* GetNextNodeRef
//------------------------------------------------------------------------------------

sInt32 CSearchNode::GetNextNodeRef ( tDirNodeReference inNodeRef, tDirNodeReference *outNodeRef, sSearchContextData *inContext )
{
	sInt32				siResult		= kEndOfSearchNodeList;
	sSearchList		   *pNodeList		= nil;
	sSearchConfig	   *aSearchConfig	= nil;
	tDirNodeReference	aNodeRef		= inNodeRef;
	uInt32				nodeIndex		= 0;

	pNodeList = (sSearchList *)inContext->fSearchNodeList;
	
	// Search the node list looking for the current node ref
	if (aNodeRef != 0) //if it is zero we look at the first one
	{
		while ( pNodeList != nil )
		{
			nodeIndex++;
			if ( aNodeRef == pNodeList->fNodeRef )
			{
				pNodeList = pNodeList->fNext;
				break;
			}
			pNodeList = pNodeList->fNext;
		}
	}

	if (nodeIndex == 1) //do this ONLY after first local node used already
	{
		//NOTE - can try this after we try the first node in the list since the most common
		//  case is we are the local domain and there is no hierarchy and the results will have been found
		//  if present already in the local node 
		//now check to see if this is the automatic search path ie. called NetInfo default for now
		//if so check to see if there is only a local node
		//if so is this another NetInfo node or no second node
		//if so then retest to determine if there is a hierarchy
		//if so reset the fSearchNodeList
		//we also reset the node list back to the beginning
		
		bool bRecheckNI = false;
		if (inContext->bAutoSearchList)
		{
			if (pNodeList != nil)
			{
				if ( pNodeList->fNodeName != nil)
				{
					if (strncmp(pNodeList->fNodeName,"/NetInfo",8) != 0)
					{
						bRecheckNI = true;
					} //second node in list is not NetInfo
				} //node name in list is non nil
			} //there is more than a single node in the search path
			else
			{
				bRecheckNI = true;
			} //no second node present
			if (bRecheckNI)
			{
				tDataList	   *pNodeNameDL	= nil;
				//call to check if there exists a NetInfo parent node
				pNodeNameDL = ::dsBuildListFromStringsPriv( "NetInfo", "..", nil );
				if (pNodeNameDL != nil)
				{
					//try to open the parent NetInfo node
					sInt32 openResult = eDSNoErr;
					openResult = dsOpenDirNode( fDirRef, pNodeNameDL, &aNodeRef );
					if ( openResult == eDSNoErr )
					{
						sSearchList	   *aSearchNodeList	= nil;
						dsCloseDirNode(aNodeRef);
						//re-evaluate the search policy now since parent CAN be opened
						//1- update search policy
						//2- set the bListChanged flags
						DoNetInfoDefault(&aSearchNodeList);
						
						if (aSearchNodeList != nil)
						{
							//switch the search policy to the current one
							
							fMutex.Wait();
	
							aSearchConfig = FindSearchConfigWithKey(inContext->fSearchConfigKey);
							
							//flush the old search path lists
							CleanSearchListData( inContext->fSearchNodeList );
							CleanSearchListData( aSearchConfig->fSearchNodeList );
							
							aSearchConfig->fSearchNodeList	= aSearchNodeList;
			
							fMutex.Signal();
							if ( aSearchConfig->pConfigFromXML == nil
								 || aSearchConfig->pConfigFromXML->IsDHCPLDAPEnabled() )
							{
								AddDefaultLDAPNodesLast(&aSearchNodeList);
							}
							//get the updated search path list with new unique refs of each
							//search path node for use by this client who opened the search node
							inContext->fSearchNodeList = DupSearchListWithNewRefs(aSearchNodeList);
							
							//all current open refs will take care of themselves but future opens will use the new list
								
							//make sure to reset the flag since doing the job right above here
							inContext->bListChanged	= false;
							
							pNodeList = ((sSearchList *)inContext->fSearchNodeList)->fNext;
						}
					}
					dsDataListDeAllocate( fDirRef, pNodeNameDL, false );
					free(pNodeNameDL);
					pNodeNameDL = nil;
				}
			}
		} //this is NetInfo default search policy
		
	} //if (nodeIndex == 1)

	//look over the remainder of the list to find the next successful open or simply finish
	while ( pNodeList != nil )
	{
		// Has the node been previously opened
		if ( pNodeList->fOpened == false )
		{
			siResult = ::dsOpenDirNode( fDirRef, pNodeList->fDataList, &pNodeList->fNodeRef );
			if ( siResult == eDSNoErr )
			{
				*outNodeRef = pNodeList->fNodeRef;
				pNodeList->fOpened = true;
				break;
			}
			else
			{
				siResult = kEndOfSearchNodeList;
			}
		}
		else
		{
			*outNodeRef	= pNodeList->fNodeRef;
			siResult	= eDSNoErr;
			break;
		}
		pNodeList = pNodeList->fNext;
	}

	return( siResult );

} // GetNextNodeRef


//------------------------------------------------------------------------------------
//	* GetNodePath
//------------------------------------------------------------------------------------

tDataList* CSearchNode::GetNodePath ( tDirNodeReference inNodeRef, sSearchContextData *inContext )
{
	tDataList	   *pResult		= nil;
	sSearchList	   *pNodeList	= nil;

//do we check ??? whether search policy has switched and if it has then adjust to the new one
//ie. in this method we are in the middle of a getrecordlist or doattributevaluesearch(withdata)
		
	pNodeList = (sSearchList *)inContext->fSearchNodeList;

	// Search the node list looking for the current node ref
	while ( pNodeList != nil )
	{
		// Is it the one we are looking for
		if ( inNodeRef == pNodeList->fNodeRef )
		{
			pResult = pNodeList->fDataList;
			break;
		}
		pNodeList = pNodeList->fNext;
	}

	return( pResult );

} // GetNodePath


// ---------------------------------------------------------------------------
//	* MakeContextData
// ---------------------------------------------------------------------------

sSearchContextData* CSearchNode::MakeContextData ( void )
{
	sSearchContextData	*pOut	= nil;

	pOut = (sSearchContextData *) ::calloc( 1, sizeof(sSearchContextData) );
	if ( pOut != nil )
	{
		pOut->fSearchNodeList	= nil;
		pOut->bListChanged		= false;
		pOut->pSearchListMutex	= nil;
		pOut->fSearchNode		= this;
		pOut->bAutoSearchList	= false;
	}

	return( pOut );

} // MakeContextData


// ---------------------------------------------------------------------------
//	* CleanContextData
// ---------------------------------------------------------------------------

sInt32 CSearchNode::CleanContextData ( sSearchContextData *inContext )
{
    sInt32				siResult 	= eDSNoErr;
	DSMutexSemaphore   *ourMutex	= nil;
    
    if (( inContext == nil ) || ( gSearchNode == nil ))
    {
        siResult = eDSBadContextData;
	}
    else
    {
		ourMutex = inContext->pSearchListMutex;
		if (ourMutex != nil)
		{
			//gSearchNode->fMutex.Wait();
			ourMutex->Wait();

			//need a handle to the pConfigFromXML class
			//cheat by using the global since all we want is the function
			//pSearchConfigList->pConfigFromXML->CleanListData( xxxxx );
        	if (inContext->fSearchNodeList != nil && inContext->fSearchNode != nil)
        	{
				gSearchNode->CleanSearchListData( inContext->fSearchNodeList );
				inContext->fSearchNodeList = nil;
			}

			inContext->bListChanged		= false;
			inContext->offset			= 0;
			inContext->fSearchConfigKey	= 0;
			inContext->pSearchListMutex	= nil;
			inContext->bAutoSearchList	= false;
			
			//ourMutex->Signal(); //we are going to delete this here - don't make it available
			delete(ourMutex);
			ourMutex = nil;
			//gSearchNode->fMutex.Signal();
		}
		//only node refs have a mutex assigned so always free this
		free( inContext );
		inContext = nil;
			

	}
		
	return( siResult );

} // CleanContextData


// --------------------------------------------------------------------------------
//	* DoAliasCheck ()
// --------------------------------------------------------------------------------

void CSearchNode::DoAliasCheck ( tDataList *inRecTypeList,  tDataList *inAttrTypeList, sSearchContinueData *inContinue )
{
	sInt32				siIndex				= 1;
	char			   *cpString			= nil;
	bool				bUserAlias			= false;
	bool				bGroupAlias			= false;
	CRecTypeList	   *clpRecTypeList		= nil;
	CAttributeList	   *clpAttrTypeList 	= nil;

	try
	{
		clpRecTypeList = new CRecTypeList( inRecTypeList );
		if ( clpRecTypeList  == nil ) throw( (sInt32)eDSEmptyRecordTypeList );
		if (clpRecTypeList->GetCount() == 0) throw( (sInt32)eDSEmptyRecordTypeList );

		while ( clpRecTypeList->GetAttribute( siIndex++, &cpString ) == eDSNoErr )
		{
			if ( (::strcmp( cpString, kDSStdRecordTypeUsers ) == 0) || (::strcmp( cpString, kDSStdUserNamesMeta ) == 0) )
			{
				bUserAlias = true;
			}
			else if ( ::strcmp( cpString, kDSStdRecordTypeGroups ) == 0 )
			{
				bGroupAlias = true;
			}
		}

		if ( (bUserAlias == true) && (bGroupAlias == true) )
		{
			inContinue->fAliasList = ::dsBuildListFromStringsPriv( kDSStdRecordTypeUserAliases, kDSStdRecordTypeGroupAliases, nil );
		}
		else if ( bUserAlias == true )
		{
			inContinue->fAliasList = ::dsBuildListFromStringsPriv( kDSStdRecordTypeUserAliases, nil );
		}
		else if ( bGroupAlias == true )
		{
			inContinue->fAliasList = ::dsBuildListFromStringsPriv( kDSStdRecordTypeGroupAliases, nil );
		}

		inContinue->fMetaTypes = keNullMetaType;

		if ( (bUserAlias == true) || (bGroupAlias == true) )
		{
			inContinue->fAliasAttribute = ::dsBuildListFromStringsPriv( kDS1AttrAliasData, nil );

			clpAttrTypeList = new CAttributeList( inAttrTypeList );
			if ( clpAttrTypeList != nil )
			{
				siIndex = 1;
				while ( clpAttrTypeList->GetAttribute( siIndex++, &cpString ) == eDSNoErr )
				{
					if ( ::strcmp( cpString, kStandardTargetAlias ) == 0 )
					{
						inContinue->fMetaTypes |= keTargetAlias;
					}
					else if ( ::strcmp( cpString, kStandardSourceAlias ) == 0 )
					{
						inContinue->fMetaTypes |= keSourceAlias;
					}
					else if ( ::strcmp( cpString, kDSAttributesAll ) == 0 )
					{
						inContinue->fMetaTypes |= keTargetAlias;
						inContinue->fMetaTypes |= keSourceAlias;
					}
				}

				delete( clpAttrTypeList );
				clpAttrTypeList = nil;
			}
		}
	}

	catch( sInt32 err )
	{
	}

	if ( clpRecTypeList != nil )
	{
		delete( clpRecTypeList );
		clpRecTypeList = nil;
	}

	if ( clpAttrTypeList != nil )
	{
		delete( clpAttrTypeList );
		clpAttrTypeList = nil;
	}

} // DoAliasCheck


// ---------------------------------------------------------------------------
//	* AddDataToOutBuff
// ---------------------------------------------------------------------------

sInt32 CSearchNode::AddDataToOutBuff ( sSearchContinueData *inContinue, CBuff *inOutBuff, sSearchContextData *inContext, tDataList *inTarget )
{
	uInt32					i				= 1;
	uInt32					j				= 1;
	sInt32					attrCnt			= 0;
	sInt32					siResult		= eDSNoErr;
	char				   *cpRecType		= nil;
	char				   *cpRecName		= nil;
	tRecordEntry		   *pRecEntry		= nil;
	tAttributeListRef		attrListRef		= 0;
	tDataList			   *pSourcePath		= nil;
	tDataList			   *pTargetPath		= nil;
	tDataNode			   *pDataNode		= nil;
	tAttributeValueListRef	valueRef		= 0;
	tAttributeEntry		   *pAttrEntry		= nil;
	tAttributeValueEntry   *pValueEntry		= nil;
	CDataBuff			   *aRecData		= nil;
	CDataBuff			   *aAttrData		= nil;
	CDataBuff			   *aTmpData		= nil;

	try
	{

		aRecData	= new CDataBuff();
		aAttrData	= new CDataBuff();
		aTmpData	= new CDataBuff();
		
		while ( (inContinue->fRecIndex <= inContinue->fRecCount) && (siResult == eDSNoErr) )
		{
			siResult = ::dsGetRecordEntry( inContinue->fNodeRef, inContinue->fDataBuff, inContinue->fRecIndex, &attrListRef, &pRecEntry );
			if ( siResult != eDSNoErr ) throw( siResult );

			siResult = ::dsGetRecordTypeFromEntry( pRecEntry, &cpRecType );
			if ( siResult != eDSNoErr ) throw( siResult );

			siResult = ::dsGetRecordNameFromEntry( pRecEntry, &cpRecName );
			if ( siResult != eDSNoErr ) throw( siResult );

			aRecData->Clear();
			aAttrData->Clear();
			aTmpData->Clear();

			// Set the record type and name
			aRecData->AppendShort( ::strlen( cpRecType ) );
			aRecData->AppendString( cpRecType );
			aRecData->AppendShort( ::strlen( cpRecName ) );
			aRecData->AppendString( cpRecName );
			//clean up this string here since it is only used here
			if ( cpRecName != nil )
			{
				free( cpRecName );
				cpRecName = nil;
			}


			if ( ((::strcmp( cpRecType, kDSStdRecordTypeUsers ) == 0) ||
				  (::strcmp( cpRecType, kDSStdUserNamesMeta ) == 0) ||
				  (::strcmp( cpRecType, kDSStdRecordTypeGroups ) == 0)) &&
				 ((inContinue->fMetaTypes & keTargetAlias) || (inContinue->fMetaTypes & keSourceAlias) ) )
			{
				attrCnt = 1;
				if ( (inContinue->fMetaTypes & keTargetAlias) && (inContinue->fMetaTypes & keSourceAlias) )
				{
					attrCnt = 2;
				}

				pSourcePath = GetNodePath( inContinue->fNodeRef, inContext );

				if ( inTarget != nil )
				{
					pTargetPath = inTarget;
				}
				else
				{
					pTargetPath = GetNodePath( inContinue->fNodeRef, inContext );
				}

				aRecData->AppendShort( pRecEntry->fRecordAttributeCount + attrCnt );

				if ( pTargetPath != nil )
				{
					if ( inContinue->fMetaTypes & keTargetAlias )
					{
						// Attribute name
						aTmpData->AppendShort( ::strlen( kStandardTargetAlias ) );
						aTmpData->AppendString( kStandardTargetAlias );

						// Append the attribute value count
						aTmpData->AppendShort( pTargetPath->fDataNodeCount );

						i = 1;
						while ( ::dsDataListGetNodeAllocPriv( pTargetPath, i++, &pDataNode ) == eDSNoErr )
						{
							aTmpData->AppendLong( ::strlen( pDataNode->fBufferData ) );
							aTmpData->AppendString( pDataNode->fBufferData );

							::dsDataBufferDeallocatePriv( pDataNode );
							pDataNode = nil;
						}

						// Add the attribute length and data
						aAttrData->AppendLong( aTmpData->GetLength() );
						aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

						// Clear the temp block
						aTmpData->Clear();
					}
				}

				if ( pSourcePath != nil )
				{
					if ( inContinue->fMetaTypes & keSourceAlias )
					{
						// Attribute name
						aTmpData->AppendShort( ::strlen( kStandardSourceAlias ) );
						aTmpData->AppendString( kStandardSourceAlias );

						// Append the attribute value count
						aTmpData->AppendShort( pSourcePath->fDataNodeCount );

						i = 1;
						while ( ::dsDataListGetNodeAllocPriv( pSourcePath, i++, &pDataNode ) == eDSNoErr )
						{
							aTmpData->AppendLong( ::strlen( pDataNode->fBufferData ) );
							aTmpData->AppendString( pDataNode->fBufferData );

							::dsDataBufferDeallocatePriv( pDataNode );
							pDataNode = nil;
						}

						// Add the attribute length and data
						aAttrData->AppendLong( aTmpData->GetLength() );
						aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

						// Clear the temp block
						aTmpData->Clear();
					}
				}
			}
			else
			{
				// Attribute count
				aRecData->AppendShort( pRecEntry->fRecordAttributeCount );
			}

			//clean up this string here since we are in a while loop and it is no longer used past the above if condition
			if ( cpRecType != nil )
			{
				free( cpRecType );
				cpRecType = nil;
			}

			if ( pRecEntry->fRecordAttributeCount != 0 )
			{
				for ( i = 1; i <= pRecEntry->fRecordAttributeCount; i++ )
				{
					siResult = ::dsGetAttributeEntry( inContinue->fNodeRef, inContinue->fDataBuff, attrListRef, i, &valueRef, &pAttrEntry );
					if ( siResult != eDSNoErr ) throw( siResult );

					aTmpData->AppendShort( ::strlen( pAttrEntry->fAttributeSignature.fBufferData ) );
					aTmpData->AppendString( pAttrEntry->fAttributeSignature.fBufferData );

					if ( inContinue->fAttrOnly == false )
					{
						aTmpData->AppendShort( pAttrEntry->fAttributeValueCount );

						for ( j = 1; j <= pAttrEntry->fAttributeValueCount; j++ )
						{
							siResult = dsGetAttributeValue( inContinue->fNodeRef, inContinue->fDataBuff, j, valueRef, &pValueEntry );
							if ( siResult != eDSNoErr ) throw( siResult );

							aTmpData->AppendLong( ::strlen( pValueEntry->fAttributeValueData.fBufferData ) );
							aTmpData->AppendString( pValueEntry->fAttributeValueData.fBufferData );
							dsDeallocAttributeValueEntry(fDirRef, pValueEntry);
							pValueEntry = nil;
						}
					}
					aAttrData->AppendLong( aTmpData->GetLength() );
					aAttrData->AppendBlock( aTmpData->GetData(), aTmpData->GetLength() );

					// Clear the temp block
					aTmpData->Clear();
					
					dsCloseAttributeValueList(valueRef);
					dsDeallocAttributeEntry(fDirRef, pAttrEntry);
					pAttrEntry = nil;
				}
			}

			if ( (pRecEntry->fRecordAttributeCount + attrCnt) != 0 )
			{
				aRecData->AppendBlock( aAttrData->GetData(), aAttrData->GetLength() );
			}

			siResult = inOutBuff->AddData( aRecData->GetData(), aRecData->GetLength() );
			if ( siResult == eDSNoErr )
			{
				inContinue->fRecIndex++;
			}
			
			dsCloseAttributeList(attrListRef);
			dsDeallocRecordEntry(fDirRef, pRecEntry);
			pRecEntry = nil;
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	if ( cpRecType != nil )
	{
		free( cpRecType );
		cpRecType = nil;
	}

	if ( cpRecName != nil )
	{
		free( cpRecName );
		cpRecName = nil;
	}

	if ( aRecData != nil )
	{
		delete(aRecData);
		aRecData = nil;
	}
	
	if ( aAttrData != nil )
	{
		delete(aAttrData);
		aAttrData = nil;
	}
	
	if ( aTmpData != nil )
	{
		delete(aTmpData);
		aTmpData = nil;
	}

	return( siResult );

} // AddDataToOutBuff


// ---------------------------------------------------------------------------
//	* ExpandAliases
// ---------------------------------------------------------------------------

sInt32 CSearchNode::ExpandAliases ( sSearchContinueData			   *inContinue,
									CBuff						   *inOutBuff,
									sGetRecordList				   *inGRLData,
									sDoAttrValueSearchWithData	   *inDAVSData,
									sSearchContextData			   *inContext )
{
	sInt32					siResult		= eDSNoErr;
	bool					done			= false;
	char				   *cpRecType		= nil;
	char				   *cpAliasType		= nil;	//do not free this ? since it is retrieved from a CFStringGetCStringPtr
	tRecordEntry		   *pRecEntry		= nil;
	tAttributeListRef		attrListRef		= 0;
	tAttributeValueListRef	valueRef		= 0;
	tAttributeEntry		   *pAttrEntry		= nil;
	tAttributeValueEntry   *pValueEntry		= nil;
	CAliases				cAlias;
	tDataList			   *pNameList		= nil;
	tDataList			   *pPathList		= nil;
	tDataList			   *pTypeList		= nil;
	tDirNodeReference		nodeRef			= 0;
	tDataBuffer			   *tDataBuff		= nil;
	tContextData			pContextData	= nil;
	sSearchContinueData		myContinue;

	try
	{
		if ((inGRLData == nil) && (inDAVSData == nil)) throw( (sInt32)eMemoryAllocError );

		if ( inGRLData != nil )
		{
			tDataBuff = ::dsDataBufferAllocatePriv( inGRLData->fInDataBuff->fBufferSize );
		}
		else
		{
			tDataBuff = ::dsDataBufferAllocatePriv( inDAVSData->fOutDataBuff->fBufferSize );
		}
		if ( tDataBuff == nil ) throw( (sInt32)eMemoryAllocError );

		::memset( &myContinue, 0, sizeof( sSearchContinueData ) );

		// KW This needs to be analyzed and revisited
		inContinue->fID++;

		while ( (inContinue->fRecIndex <= inContinue->fRecCount) && (siResult == eDSNoErr) && !done )
		{
			// Get the record
			siResult = ::dsGetRecordEntry( inContinue->fNodeRef, inContinue->fDataBuff, inContinue->fRecIndex, &attrListRef, &pRecEntry );
			if ( siResult == eDSNoErr )
			{
				// Get the record type
				siResult = ::dsGetRecordTypeFromEntry( pRecEntry, &cpRecType );
				if ( (siResult == eDSNoErr) && (pRecEntry->fRecordAttributeCount != 0) )
				{
					// Get the first attribute
					siResult = ::dsGetAttributeEntry( inContinue->fNodeRef, inContinue->fDataBuff, attrListRef, 1, &valueRef, &pAttrEntry );
					if ( siResult == eDSNoErr )
					{
						// Is it what we expected
						if ( ::strcmp( pAttrEntry->fAttributeSignature.fBufferData, kDS1AttrAliasData ) == 0 )
						{
							// Get the first attribute value
							siResult = ::dsGetAttributeValue( inContinue->fNodeRef, inContinue->fDataBuff, 1, valueRef, &pValueEntry );
						}
						else
						{
							siResult = eDSInvalidIndex;
						}
					}
					dsCloseAttributeValueList(valueRef);
					if (pAttrEntry != nil)
					{
						dsDeallocAttributeEntry(fDirRef, pAttrEntry);
						pAttrEntry = nil;
					}
				}
				else
				{
					siResult = eDSInvalidIndex;
				}
			}

			if ( siResult == eDSNoErr )
			{
				siResult = cAlias.Initialize( pValueEntry->fAttributeValueData.fBufferData, pValueEntry->fAttributeValueData.fBufferLength );
				if ( siResult == eDSNoErr )
				{
					siResult = cAlias.GetRecordType( &cpAliasType );
					if ( siResult == eDSNoErr )
					{
						if ( (::strcmp( cpRecType, kDSStdRecordTypeUserAliases ) == 0) &&
							 (::strcmp( cpAliasType, kDSStdRecordTypeUsers ) == 0) )
						{
							pTypeList = ::dsBuildListFromStringsPriv( kDSStdRecordTypeUsers, nil );
						}
						else if ( (::strcmp( cpRecType, kDSStdRecordTypeGroupAliases ) == 0) &&
								  (::strcmp( cpAliasType, kDSStdRecordTypeGroups ) == 0) )
						{
							pTypeList = ::dsBuildListFromStringsPriv( kDSStdRecordTypeGroups, nil );
						}
						else
						{
							siResult = eDSInvalidIndex;
						}
					}

					inContinue->fRecIndex++;
					done = true;
					if ( siResult == eDSNoErr )
					{
						siResult = eMemoryAllocError;
						pNameList = ::dsDataListAllocatePriv();
						if ( pNameList != nil )
						{
							siResult = cAlias.GetRecordName( pNameList );
							if ( siResult == eDSNoErr )
							{
								siResult = eMemoryAllocError;
								pPathList = ::dsDataListAllocatePriv();
								if ( pPathList != nil )
								{
									siResult = cAlias.GetRecordLocation( pPathList );
									if ( siResult == eDSNoErr )
									{
										siResult = ::dsOpenDirNode( fDirRef, pPathList, &nodeRef );
										if ( siResult == eDSNoErr )
										{
											if ( inGRLData != nil )
											{
												siResult = ::dsGetRecordList( nodeRef,
																				tDataBuff,
																				pNameList,
																				inGRLData->fInPatternMatch,
																				pTypeList,
																				inGRLData->fInAttribTypeList,
																				inGRLData->fInAttribInfoOnly,
																				&myContinue.fRecCount,
																				&pContextData );
											}
											else
											{
												if ( inDAVSData->fType == kDoAttributeValueSearchWithData )
												{
													siResult = ::dsDoAttributeValueSearchWithData(
																				nodeRef,
																				tDataBuff,
																				pTypeList,
																				inDAVSData->fInAttrType,
																				inDAVSData->fInPattMatchType,
																				inDAVSData->fInPatt2Match,
																				inDAVSData->fInAttrTypeRequestList,
																				inDAVSData->fInAttrInfoOnly,
																				&myContinue.fRecCount,
																				&pContextData );
												}
												else
												{
													siResult = ::dsDoAttributeValueSearch(
																				nodeRef,
																				tDataBuff,
																				pTypeList,
																				inDAVSData->fInAttrType,
																				inDAVSData->fInPattMatchType,
																				inDAVSData->fInPatt2Match,
																				&myContinue.fRecCount,
																				&pContextData );
												}
											}

											if ( siResult == eDSNoErr )
											{
												myContinue.fNodeRef		= inContinue->fNodeRef;
												myContinue.fRecIndex	= 1;
												myContinue.fRecCount	= 1;
												if ( inGRLData != nil )
												{
													myContinue.fAttrOnly	= inGRLData->fInAttribInfoOnly;
												}
												myContinue.fMetaTypes	= inContinue->fMetaTypes;
												myContinue.fDataBuff	= tDataBuff;

												siResult = AddDataToOutBuff( &myContinue, inOutBuff, inContext, pPathList );
												if ( siResult == CBuff::kBuffFull )
												{
													inContinue->fRecIndex--;
													done = true;
												}
												else
												{
													done = false;
												}
											}
											::dsCloseDirNode( nodeRef );
										}
									}
									(void)::dsDataListDeallocatePriv( pPathList );
									//need to free the header as well
									free( pPathList );
									pPathList = nil;
								}
							}
							(void)::dsDataListDeallocatePriv( pNameList );
							//need to free the header as well
							free( pNameList );
							pNameList = nil;
						}
						(void)::dsDataListDeallocatePriv( pTypeList );
						//need to free the header as well
						free( pTypeList );
						pTypeList = nil;
					}
				}
			}
			if (pValueEntry != nil)
			{
				dsDeallocAttributeValueEntry(fDirRef, pValueEntry);
				pValueEntry = nil;
			}

			// remove last rec type since inside the while we get the next one
			if ( cpRecType != nil )
			{
				free( cpRecType );
				cpRecType = nil;
			}

			dsCloseAttributeList(attrListRef);
			dsDeallocRecordEntry(fDirRef, pRecEntry);
			pRecEntry = nil;
		} // while loop over records

		if ( tDataBuff != nil )
		{
			::dsDataBufferDeallocatePriv( tDataBuff );
			tDataBuff = nil;
		}
	}

	catch( sInt32 err )
	{
		siResult = err;
	}

	if ( cpRecType != nil )
	{
		free( cpRecType );
		cpRecType = nil;
	}
	
	return( eDSNoErr );

} // ExpandAliases

//------------------------------------------------------------------------------------
//	  * DoPlugInCustomCall
//------------------------------------------------------------------------------------ 

sInt32 CSearchNode::DoPlugInCustomCall ( sDoPlugInCustomCall *inData )
{
	sInt32				siResult		= eDSNoErr;
	unsigned long		aRequest		= 0;
	sInt32				xmlDataLength	= 0;
	CFDataRef   		xmlData			= nil;
	CFDictionaryRef		dhcpLDAPdict	= nil;
	CFMutableArrayRef	cspArray		= nil;
	unsigned long		bufLen			= 0;
	sSearchContextData *pContext		= nil;
	sSearchConfig	   *aSearchConfig	= nil;
	AuthorizationRef	authRef			= 0;
	AuthorizationItemSet* resultRightSet = NULL;

	try
	{
		if ( inData == nil ) throw( (sInt32)eDSNullParameter );
		if ( inData->fInRequestData == nil ) throw( (sInt32)eDSNullDataBuff );
		if ( inData->fInRequestData->fBufferData == nil ) throw( (sInt32)eDSEmptyBuffer );
			
		pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInNodeRef );
		if ( pContext == nil ) throw( (sInt32)eDSInvalidNodeRef );
		
		//stop the call if the call comes in for the DefaultNetwork Node
		if (pContext->fSearchConfigKey == eDSNetworkSearchNodeName)  throw( (sInt32)eDSInvalidNodeRef );

		aRequest = inData->fInRequestCode;
		bufLen = inData->fInRequestData->fBufferLength;
		if ( bufLen < sizeof( AuthorizationExternalForm ) ) throw( (sInt32)eDSInvalidBuffFormat );
		siResult = AuthorizationCreateFromExternalForm((AuthorizationExternalForm *)inData->fInRequestData->fBufferData,
			&authRef);
		if (siResult != errAuthorizationSuccess)
		{
			throw( (sInt32)eDSPermissionError );
		}

		AuthorizationItem rights[] = { {"system.services.directory.configure", 0, 0, 0} };
		AuthorizationItemSet rightSet = { sizeof(rights)/ sizeof(*rights), rights };
		
		siResult = AuthorizationCopyRights(authRef, &rightSet, NULL,
			kAuthorizationFlagExtendRights, &resultRightSet);
		if (resultRightSet != NULL)
		{
			AuthorizationFreeItemSet(resultRightSet);
			resultRightSet = NULL;
		}
		if (siResult != errAuthorizationSuccess)
		{
			throw( (sInt32)eDSPermissionError );
		}

		fMutex.Wait();
		aSearchConfig	= FindSearchConfigWithKey(pContext->fSearchConfigKey);
		if ( aSearchConfig == nil ) throw( (sInt32)eDSInvalidNodeRef );

		// Set it to the first node in the search list - this check doesn't need to use the context search path
		if ( aSearchConfig->fSearchNodeList == nil ) throw( (sInt32)eSearchPathNotDefined );

		switch( aRequest )
		{
			case 111:
				SwitchSearchPolicy( kNetInfoSearchPolicy, aSearchConfig );
				//need to save the switch to the config file
				if (aSearchConfig->pConfigFromXML)
				{
					siResult = aSearchConfig->pConfigFromXML->SetSearchPolicy(kNetInfoSearchPolicy);
					siResult = aSearchConfig->pConfigFromXML->WriteConfig();
				}
				break;

			case 222:
				SwitchSearchPolicy( kLocalSearchPolicy, aSearchConfig );
				//need to save the switch to the config file
				if (aSearchConfig->pConfigFromXML)
				{
					siResult = aSearchConfig->pConfigFromXML->SetSearchPolicy(kLocalSearchPolicy);
					siResult = aSearchConfig->pConfigFromXML->WriteConfig();
				}
				break;

			case 333:
				SwitchSearchPolicy( kCustomSearchPolicy, aSearchConfig );
				//need to save the switch to the config file
				if (aSearchConfig->pConfigFromXML)
				{
					siResult = aSearchConfig->pConfigFromXML->SetSearchPolicy(kCustomSearchPolicy);
					siResult = aSearchConfig->pConfigFromXML->WriteConfig();
				}
				break;

				//here we accept an XML blob to replace the current custom search path nodes
			case 444:
				//need to make xmlData large enough to receive the data
				//the XML data immediately follows the AuthorizationExternalForm
				xmlDataLength = (sInt32) bufLen - sizeof( AuthorizationExternalForm );
				if ( xmlDataLength <= 0 ) throw( (sInt32)eDSInvalidBuffFormat );
				
				xmlData = CFDataCreate(NULL,(UInt8 *)(inData->fInRequestData->fBufferData + sizeof( AuthorizationExternalForm )),xmlDataLength);
				//build the csp array
	   			cspArray = (CFMutableArrayRef)CFPropertyListCreateFromXMLData(NULL,xmlData,0,NULL);
				if (aSearchConfig->pConfigFromXML)
				{
					siResult = aSearchConfig->pConfigFromXML->SetListArray(cspArray);
					siResult = aSearchConfig->pConfigFromXML->WriteConfig();
				}
				CFRelease(cspArray);
	   			CFRelease(xmlData);
				// need to reset the policy since changes made to the data need to be picked up
				SwitchSearchPolicy( kCustomSearchPolicy, aSearchConfig );
				break;

			case 555:
				// get length of DHCP LDAP dictionary

				if ( inData->fOutRequestResponse == nil ) throw( (sInt32)eDSNullDataBuff );
				if ( inData->fOutRequestResponse->fBufferData == nil ) throw( (sInt32)eDSEmptyBuffer );
				if ( inData->fOutRequestResponse->fBufferSize < sizeof( CFIndex ) ) throw( (sInt32)eDSInvalidBuffFormat );
				if ( aSearchConfig->pConfigFromXML != nil)
				{
					// need four bytes for size
					dhcpLDAPdict = aSearchConfig->pConfigFromXML->GetDHCPLDAPDictionary();
					if (dhcpLDAPdict != 0)
					{
						xmlData = CFPropertyListCreateXMLData(NULL,dhcpLDAPdict);
					}
					if (xmlData != 0)
					{
						*(CFIndex*)(inData->fOutRequestResponse->fBufferData) = CFDataGetLength(xmlData);
						inData->fOutRequestResponse->fBufferLength = sizeof( CFIndex );
						CFRelease(xmlData);
						xmlData = 0;
					}
					else
					{
						*(CFIndex*)(inData->fOutRequestResponse->fBufferData) = 0;
						inData->fOutRequestResponse->fBufferLength = sizeof( CFIndex );
					}
				}
				break;

			case 556:
				// read xml config
				CFRange	aRange;

				if ( inData->fOutRequestResponse == nil ) throw( (sInt32)eDSNullDataBuff );
				if ( inData->fOutRequestResponse->fBufferData == nil ) throw( (sInt32)eDSEmptyBuffer );
				if ( aSearchConfig->pConfigFromXML != nil )
				{
					dhcpLDAPdict = aSearchConfig->pConfigFromXML->GetDHCPLDAPDictionary();
					if (dhcpLDAPdict != 0)
					{
						xmlData = CFPropertyListCreateXMLData(NULL,dhcpLDAPdict);
					}
					if (xmlData != 0)
					{
						aRange.location = 0;
						aRange.length = CFDataGetLength(xmlData);
						if ( inData->fOutRequestResponse->fBufferSize < (unsigned int)aRange.length ) throw( (sInt32)eDSBufferTooSmall );
						CFDataGetBytes( xmlData, aRange, (UInt8*)(inData->fOutRequestResponse->fBufferData) );
						inData->fOutRequestResponse->fBufferLength = aRange.length;
						CFRelease(xmlData);
						xmlData = 0;
					}
				}
				break;
				
			case 557:
				//need to make xmlData large enough to receive the data
				//the XML data immediately follows the AuthorizationExternalForm
				xmlDataLength = (sInt32) bufLen - sizeof( AuthorizationExternalForm );
				if ( xmlDataLength <= 0 ) throw( (sInt32)eDSInvalidBuffFormat );

				xmlData = CFDataCreate(NULL,(UInt8 *)(inData->fInRequestData->fBufferData + sizeof( AuthorizationExternalForm )),xmlDataLength);
				//build the csp array
				dhcpLDAPdict = (CFDictionaryRef)CFPropertyListCreateFromXMLData(NULL,xmlData,0,NULL);
				if (aSearchConfig->pConfigFromXML)
				{
					aSearchConfig->pConfigFromXML->SetDHCPLDAPDictionary(dhcpLDAPdict);
					siResult = aSearchConfig->pConfigFromXML->WriteConfig();
				}
				CFRelease(dhcpLDAPdict);
				CFRelease(xmlData);
				// need to reset the policy since changes made to the data need to be picked up
				if (aSearchConfig->fSearchConfigKey == kNetInfoSearchPolicy)
				{
					// need to make sure we pick up any changes if automatic search policy is active
					SwitchSearchPolicy( aSearchConfig->fSearchConfigKey, aSearchConfig );
				}
				break;
				
			default:
  				break;
		}
		fMutex.Signal();

	}

	catch( sInt32 err )
	{
		fMutex.Signal();
		siResult = err;
	}

	if (authRef != 0)
	{
		AuthorizationFree(authRef, 0);
		authRef = 0;
	}

	return( siResult );

} // DoPlugInCustomCall


// ---------------------------------------------------------------------------
//	* CleanSearchConfigData
// ---------------------------------------------------------------------------

sInt32 CSearchNode:: CleanSearchConfigData ( sSearchConfig *inList )
{
    sInt32				siResult	= eDSNoErr;

    if ( inList != nil )
    {
		inList->fSearchPolicy			= 0;
		inList->fSearchConfigKey		= 0;
		inList->fDirNodeType			= kUnknownNodeType;

		inList->fNext					= nil;
		
		if (inList->fSearchNodeName != nil)
		{
			free(inList->fSearchNodeName);
			inList->fSearchNodeName = nil;
		}
		if (inList->fSearchConfigFilePrefix != nil)
		{
			free(inList->fSearchConfigFilePrefix);
			inList->fSearchConfigFilePrefix = nil;
		}
		
		CleanSearchListData( inList->fSearchNodeList );
		
		inList->fSearchNodeList = nil; //take the chance this is a leak if inList->pConfigFromXML was nil
		
		if (inList->pConfigFromXML != nil)
		{
			delete(inList->pConfigFromXML);
			inList->pConfigFromXML = nil;
		}
   }

    return( siResult );

} // CleanSearchConfigData


// ---------------------------------------------------------------------------
//	* CleanSearchListData
// ---------------------------------------------------------------------------

sInt32 CSearchNode:: CleanSearchListData ( sSearchList *inList )
{
    sInt32				siResult	= eDSNoErr;
	sSearchList		   *pList		= nil;
	sSearchList		   *pDeleteList	= nil;


	if (inList != nil)
	{
		//need the nested ifs so that can make use of the CleanListData method
		//need to cleanup the struct list ie. the internals
		pList = inList;
		while (pList != nil)
		{
			pDeleteList = pList;
			pList = pList->fNext;		//assign to next BEFORE deleting current
			if (pDeleteList->fNodeName != nil)
			{
				delete ( pDeleteList->fNodeName );
			}
			pDeleteList->fOpened = false;
			if (pDeleteList->fNodeRef != 0)
			{
				::dsCloseDirNode(pDeleteList->fNodeRef); // don't check error code
				pDeleteList->fNodeRef = 0;
			}
			pDeleteList->fNext = nil;
			if (pDeleteList->fDataList != nil)
			{
				dsDataListDeallocatePriv ( pDeleteList->fDataList );
				//need to free the header as well
				free( pDeleteList->fDataList );
				pDeleteList->fDataList = nil;
			}
			delete( pDeleteList );
			pDeleteList = nil;
		}
	}

    return( siResult );

} // CleanSearchListData

// ---------------------------------------------------------------------------
//	* MakeSearchConfigData
// ---------------------------------------------------------------------------

sSearchConfig *CSearchNode::MakeSearchConfigData (	sSearchList *inSearchNodeList,
													uInt32 inSearchPolicy,
													CConfigs *inConfigFromXML,
													char *inSearchNodeName,
													char *inSearchConfigFilePrefix,
													eDirNodeType inDirNodeType,
													uInt32 inSearchConfigType )
{
    sInt32				siResult		= eDSNoErr;
    sSearchConfig  	   *configOut		= nil;

	configOut = (sSearchConfig *) ::calloc(sizeof(sSearchConfig), sizeof(char));
	if (configOut != nil)
	{
		//just created so no need to check siResult?
		siResult = CleanSearchConfigData(configOut);
		configOut->fSearchNodeList			= inSearchNodeList;
		configOut->fSearchPolicy			= inSearchPolicy;
		configOut->pConfigFromXML			= inConfigFromXML;
		configOut->fSearchNodeName			= inSearchNodeName;
		configOut->fSearchConfigFilePrefix	= inSearchConfigFilePrefix;
		configOut->fDirNodeType				= inDirNodeType;
		configOut->fSearchConfigKey			= inSearchConfigType;
		configOut->fNext					= nil;
	}

    return( configOut );

} // MakeSearchConfigData


// ---------------------------------------------------------------------------
//	* FindSearchConfigWithKey
// ---------------------------------------------------------------------------

sSearchConfig *CSearchNode:: FindSearchConfigWithKey (	uInt32 inSearchConfigKey )
{
    sSearchConfig  	   *configOut		= nil;

	fMutex.Wait();
	configOut = pSearchConfigList;
	while ( configOut != nil )
	{
		if (configOut->fSearchConfigKey == inSearchConfigKey)
		{
			break;
		}
		configOut = configOut->fNext;
	}
	fMutex.Signal();

    return( configOut );

} // FindSearchConfigWithKey


// ---------------------------------------------------------------------------
//	* AddSearchConfigToList
// ---------------------------------------------------------------------------

sInt32 CSearchNode:: AddSearchConfigToList ( sSearchConfig *inSearchConfig )
{
    sSearchConfig  	   *aConfigList		= nil;
	sInt32				siResult		= eDSInvalidIndex;
	bool				uiDup			= false;

	fMutex.Wait();
	aConfigList = pSearchConfigList;
	while ( aConfigList != nil ) // look for existing entry with same key
	{
		if (aConfigList->fSearchConfigKey == inSearchConfig->fSearchConfigKey)
		{
			uiDup = true;
			break;
		}
		aConfigList = aConfigList->fNext;
	}

	if (!uiDup) //don't add if entry already exists
	{
		aConfigList = pSearchConfigList;
		if (aConfigList == nil)
		{
			pSearchConfigList = inSearchConfig;
		}
		else
		{
			while ( aConfigList->fNext != nil )
			{
				aConfigList = aConfigList->fNext;
			}
			aConfigList->fNext = inSearchConfig;
		}
		siResult = eDSNoErr;
	}
	fMutex.Signal();
	
    return( siResult );

} // AddSearchConfigToList


// ---------------------------------------------------------------------------
//	* RemoveSearchConfigWithKey //TODO this could be a problem if it is ever called
// ---------------------------------------------------------------------------
/*
sInt32 CSearchNode:: RemoveSearchConfigWithKey ( uInt32 inSearchConfigKey )
{
    sSearchConfig  	   *aConfigList		= nil;
    sSearchConfig  	   *aConfigPtr		= nil;
	sInt32				siResult		= eDSInvalidIndex;

	fMutex.Wait();
	aConfigList = pSearchConfigList;
	aConfigPtr	= pSearchConfigList;
	if (aConfigList->fSearchConfigKey == inSearchConfigKey)
	{
		pSearchConfigList = aConfigList->fNext;
		siResult = eDSNoErr;
	}
	else
	{
		aConfigList = aConfigList->fNext;
		while ( aConfigList != nil ) // look for existing entry with same key
		{
			if (aConfigList->fSearchConfigKey == inSearchConfigKey)
			{
				aConfigPtr->fNext = aConfigList->fNext;
				siResult = eDSNoErr;
				break;
			}
			aConfigList = aConfigList->fNext;
			aConfigPtr	= aConfigPtr->fNext;
		}
	}
	fMutex.Signal();
	
    return( siResult );

} // RemoveSearchConfigWithKey
*/

//------------------------------------------------------------------------------------
//	* CloseAttributeList
//------------------------------------------------------------------------------------

sInt32 CSearchNode::CloseAttributeList ( sCloseAttributeList *inData )
{
	sInt32				siResult		= eDSNoErr;
	sSearchContextData *pContext		= nil;

	pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInAttributeListRef );
	if ( pContext != nil )
	{
		//only "offset" should have been used in the Context
		gSNNodeRef->RemoveItem( inData->fInAttributeListRef );
	}
	else
	{
		siResult = eDSInvalidAttrListRef;
	}

	return( siResult );

} // CloseAttributeList


//------------------------------------------------------------------------------------
//	* CloseAttributeValueList
//------------------------------------------------------------------------------------

sInt32 CSearchNode::CloseAttributeValueList ( sCloseAttributeValueList *inData )
{
	sInt32				siResult		= eDSNoErr;
	sSearchContextData *pContext		= nil;

	pContext = (sSearchContextData *)gSNNodeRef->GetItemData( inData->fInAttributeValueListRef );
	if ( pContext != nil )
	{
		//only "offset" should have been used in the Context
		gSNNodeRef->RemoveItem( inData->fInAttributeValueListRef );
	}
	else
	{
		siResult = eDSInvalidAttrValueRef;
	}

	return( siResult );

} // CloseAttributeValueList


//------------------------------------------------------------------------------------
//	* DupSearchListWithNewRefs
//------------------------------------------------------------------------------------

sSearchList *CSearchNode::DupSearchListWithNewRefs ( sSearchList *inSearchList )
{
	sSearchList	   *outSearchList		= nil;
	sSearchList	   *pSearchList			= inSearchList;
	sSearchList	   *aSearchList			= nil;
	sSearchList	   *tailSearchList		= nil;
	bool			isFirst				= true;
	bool			getLocalFirst  		= true;
//	tDataList	   *pLocalNodeName		= nil;
//	sInt32			siResult			= eDSNoErr;

	//this might be a good place to refresh (re-init?) the search policy to get a fresh perspective on the search paths?
	while (pSearchList != nil)
	{
   		aSearchList = (sSearchList *)::calloc( 1, sizeof( sSearchList ) );
		
		//init
		aSearchList->fOpened	= false;
		aSearchList->fNodeRef	= 0;
		aSearchList->fNodeName	= nil;
		aSearchList->fDataList	= nil;
		aSearchList->fNext		= nil;

		//need to retain the order
		if (isFirst)
		{
			outSearchList		= aSearchList;
			tailSearchList		= aSearchList;
			isFirst				= false;
		}
		else
		{
			tailSearchList->fNext	= aSearchList;
			tailSearchList			= aSearchList;
		}

		if (pSearchList->fNodeName != nil)
		{
			aSearchList->fNodeName = (char *)::calloc(1, ::strlen(pSearchList->fNodeName) + 1);
			::strcpy(aSearchList->fNodeName,pSearchList->fNodeName);

			//aSearchList->fDataList = ::dsBuildFromPathPriv( aSearchList->fNodeName, "/" );
			
			if (getLocalFirst)
			{
				aSearchList->fDataList = ::dsBuildFromPathPriv( kstrDefaultLocalNodeName, "/" );

				//let's open lazily when we actually need the node ref
				getLocalFirst = false;
			}
			else
			{
				aSearchList->fDataList = ::dsBuildFromPathPriv( aSearchList->fNodeName, "/" );
			
				//let's open lazily when we actually need the node ref
			}
		}

		pSearchList = pSearchList->fNext;
	}

	return( outSearchList );

} // DupSearchListWithNewRefs


// ---------------------------------------------------------------------------
//	* ContinueDeallocProc
// ---------------------------------------------------------------------------

void CSearchNode::ContinueDeallocProc ( void* inContinueData )
{
	sSearchContinueData *pContinue = (sSearchContinueData *)inContinueData;

	if ( pContinue != nil )
	{
		if ( pContinue->fAliasList != nil )
		{
			::dsDataListDeallocatePriv( pContinue->fAliasList );
			//need to free the header as well
			free( pContinue->fAliasList );
			pContinue->fAliasList = nil;
		}

		if ( pContinue->fAliasAttribute != nil )
		{
			::dsDataListDeallocatePriv( pContinue->fAliasAttribute );
			//need to free the header as well
			free( pContinue->fAliasAttribute );
			pContinue->fAliasAttribute = nil;
		}

		if ( pContinue->fDataBuff != nil )
		{
			::dsDataBufferDeallocatePriv( pContinue->fDataBuff );
			pContinue->fDataBuff = nil;
		}

		if ( pContinue->fContextData != nil )
		{
			::dsReleaseContinueData( pContinue->fNodeRef, pContinue->fContextData );
			pContinue->fContextData = nil;
		}

		free( pContinue );
		pContinue = nil;
	}
} // ContinueDeallocProc


// ---------------------------------------------------------------------------
//	* ContextDeallocProc
// ---------------------------------------------------------------------------

void CSearchNode::ContextDeallocProc ( void* inContextData )
{
	sSearchContextData *pContext = (sSearchContextData *) inContextData;

	if ( pContext != nil )
	{
		CleanContextData( pContext );
	}
} // ContextDeallocProc

// ---------------------------------------------------------------------------
//	* ContextSetListChangedProc
// ---------------------------------------------------------------------------

void CSearchNode:: ContextSetListChangedProc ( void* inContextData )
{
	sSearchContextData *pContext = (sSearchContextData *) inContextData;

	if ( pContext != nil )
	{
		if ( pContext->pSearchListMutex != nil ) //this not nil is an indicator that this is a node reference context
		{
			pContext->pSearchListMutex->Wait();
			pContext->bListChanged	= true;
			pContext->pSearchListMutex->Signal();
		}
	}
} // ContextSetListChangedProc


// ---------------------------------------------------------------------------
//	* SetSearchPolicyIndicatorFile -- ONLY used with AuthenticationSearch Node
// ---------------------------------------------------------------------------

void CSearchNode:: SetSearchPolicyIndicatorFile ( uInt32 inSearchNodeKey, uInt32 inSearchPolicyIndex )
{
	sInt32			siResult	= eDSNoErr;
	struct stat		statResult;

	if (inSearchNodeKey == eDSAuthenticationSearchNodeName)
	{
		//check if the directory exists that holds the indicator file
		siResult = ::stat( "/Library/Preferences/DirectoryService", &statResult );
		if (siResult != eDSNoErr)
		{
			siResult = ::stat( "/Library/Preferences", &statResult );
			//if first sub directory does not exist
			if (siResult != eDSNoErr)
			{
				::mkdir( "/Library/Preferences", 0775 );
				::chmod( "/Library/Preferences", 0775 ); //above 0775 doesn't seem to work - looks like umask modifies it
			}
			siResult = ::stat( "/Library/Preferences/DirectoryService", &statResult );
			//if second sub directory does not exist
			if (siResult != eDSNoErr)
			{
				::mkdir( "/Library/Preferences/DirectoryService", 0775 );
				::chmod( "/Library/Preferences/DirectoryService", 0775 ); //above 0775 doesn't seem to work - looks like umask modifies it
			}
		}
		
		//eliminate the existing indicator file
		RemoveSearchPolicyIndicatorFile();
		
		//add the new indicator file
		if (inSearchPolicyIndex == 3)
		{
			system( "touch /Library/Preferences/DirectoryService/.DSRunningSP3" );
		}
		else if (inSearchPolicyIndex == 2)
		{
			system( "touch /Library/Preferences/DirectoryService/.DSRunningSP2" );
		}
		else //assume inSearchPolicyIndex = 1
		{
			system( "touch /Library/Preferences/DirectoryService/.DSRunningSP1" );
		}
	}

} // SetSearchPolicyIndicatorFile

// ---------------------------------------------------------------------------
//	* RemoveSearchPolicyIndicatorFile
// ---------------------------------------------------------------------------

void CSearchNode:: RemoveSearchPolicyIndicatorFile ( void )
{
	sInt32			siResult	= eDSNoErr;
	struct stat		statResult;

	siResult = ::stat( "/Library/Preferences/DirectoryService/.DSRunningSP1", &statResult );
	//if file exists delete it
	if (siResult == eDSNoErr)
	{
		system( "rm -f /Library/Preferences/DirectoryService/.DSRunningSP1" );
	}
	
	siResult = ::stat( "/Library/Preferences/DirectoryService/.DSRunningSP2", &statResult );
	//if file exists delete it
	if (siResult == eDSNoErr)
	{
		system( "rm -f /Library/Preferences/DirectoryService/.DSRunningSP2" );
	}
	
	siResult = ::stat( "/Library/Preferences/DirectoryService/.DSRunningSP3", &statResult );
	//if file exists delete it
	if (siResult == eDSNoErr)
	{
		system( "rm -f /Library/Preferences/DirectoryService/.DSRunningSP3" );
	}
	
} // RemoveSearchPolicyIndicatorFile


//--------------------------------------------------------------------------------------------------
// * BuildNetworkNodeList ()
//--------------------------------------------------------------------------------------------------

sSearchList *CSearchNode::BuildNetworkNodeList ( void )
{
	sSearchList	   *outSearchList	= nil;
	sSearchList	   *aSearchList		= nil;
	sSearchList	   *tailSearchList	= nil;
	bool			isFirst			= true;
	tDataBuffer	   *pNodeBuff 		= nil;
	bool			done			= false;
	unsigned long	uiCount			= 0;
	unsigned long	uiIndex			= 0;
	tContextData	context			= NULL;
	tDataList	   *pDataList		= nil;
	sInt32			siResult		= eDSNoErr;

// alloc a buffer
// find dir nodes of default network type
// set only the path str and the tDataList
// since we open nodes lazily
// add to the list

	try
	{
		pNodeBuff	= ::dsDataBufferAllocatePriv( 2048 );
		if ( pNodeBuff == nil ) throw( (sInt32)eMemoryAllocError );
			
		while ( done == false )
		{
			do 
			{
				siResult = dsFindDirNodes( fDirRef, pNodeBuff, NULL, eDSDefaultNetworkNodes, &uiCount, &context );
				if (siResult == eDSBufferTooSmall)
				{
					uInt32 bufSize = pNodeBuff->fBufferSize;
					dsDataBufferDeallocatePriv( pNodeBuff );
					pNodeBuff = nil;
					pNodeBuff = ::dsDataBufferAllocatePriv( bufSize * 2 );
				}
			} while (siResult == eDSBufferTooSmall);
		
			if ( siResult != eDSNoErr ) throw( siResult );
			
			for ( uiIndex = 1; uiIndex <= uiCount; uiIndex++ )
			{
				siResult = dsGetDirNodeName( fDirRef, pNodeBuff, uiIndex, &pDataList );
				if ( siResult != eDSNoErr ) throw( siResult );
				
				//here we have the node name in a tDataList
				//NOW build the search list item
				aSearchList = (sSearchList *)::calloc( 1, sizeof( sSearchList ) );
				if ( aSearchList == nil ) throw( (sInt32)eMemoryAllocError );
				
				//init
				aSearchList->fOpened	= false;
				aSearchList->fNodeRef	= 0;
				aSearchList->fDataList	= pDataList;
				//get path str from tDatalist
				aSearchList->fNodeName	= dsGetPathFromListPriv( pDataList, "/" );
				aSearchList->fNext		= nil;
		
				//retaining the ordering from dsFindDirNodes
				if (isFirst)
				{
					outSearchList		= aSearchList;
					tailSearchList		= aSearchList;
					isFirst				= false;
				}
				else
				{
					tailSearchList->fNext	= aSearchList;
					tailSearchList			= aSearchList;
				}
		
						
				//the pDataList is consumed by the aSearchList so don't dealloc it
				//siResult = dsDataListDeallocatePriv( pDataList );
				//if ( siResult != eDSNoErr ) throw( siResult );
				//free(pDataList);
				pDataList = nil;
				
			} // for loop over uiIndex
			
			done = (context == nil);

		} // while done == false
		
		dsDataBufferDeallocatePriv( pNodeBuff );
		pNodeBuff = nil;
		
	} // try

	catch( sInt32 err )
	{
		//KW might try to clean up outSearchList here but hard to know where the memory alloc above failed
		outSearchList = nil;
		CShared::LogIt( 0x0F, "Memory error finding the Default Network Nodes with error: %l", err );
	}

	return( outSearchList );

} // BuildNetworkNodeList

