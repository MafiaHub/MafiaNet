//
// This file was taken from RakNet 4.082.
// Please see licenses/RakNet license.txt for the underlying license and related copyright.
//
//
//
// Modified work: Copyright (c) 2018, SLikeSoft UG (haftungsbeschränkt)
//
// This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
// license found in the license.txt file in the root directory of this source tree.

//----------------------------Method modifiers---------------------
//These modify the method types by default it uses just publix x, here you can specify ovverideds private functions
//and such. Many times this is used to hide the helper functions from the user

//BitStream
%csmethodmodifiers SLNet::BitStream::CSharpStringReader "private"
%csmethodmodifiers SLNet::BitStream::CSharpStringReaderCompressedDelta "private"
%csmethodmodifiers SLNet::BitStream::CSharpStringReaderCompressed "private"
%csmethodmodifiers SLNet::BitStream::CSharpStringReaderDelta "private"
%csmethodmodifiers SLNet::BitStream::CSharpByteReader(unsigned char* inOutByteArray,unsigned int numberOfBytes) "private"
%csmethodmodifiers SLNet::BitStream::CSharpCopyDataHelper(unsigned char* inOutByteArray) "private"
%csmethodmodifiers SLNet::BitStream::CSharpPrintHexHelper(char * inString) "private"
%csmethodmodifiers SLNet::BitStream::CSharpPrintBitsHelper(char * inString) "private"

//DataStructures::Table
%csmethodmodifiers DataStructures::Table::GetListHeadHelper "private"
%csmethodmodifiers DataStructures::Table::SortTableHelper "private"
%csmethodmodifiers DataStructures::Table::GetCellValueByIndexHelper "private"
%csmethodmodifiers DataStructures::Table::QueryTableHelper "private"
%csmethodmodifiers DataStructures::Table::ColumnIndexHelper "private"
%csmethodmodifiers Cell::GetHelper "private"
%csmethodmodifiers Cell::ColumnIndexHelper "private"

//Rakpeer
%define RAKPEERANDINTERFACESETPRIVATE(theMacroInputFunction)
%csmethodmodifiers SLNet::RakPeer::theMacroInputFunction "private"
%csmethodmodifiers SLNet::RakPeerInterface::theMacroInputFunction "private"
%enddef

RAKPEERANDINTERFACESETPRIVATE(CSharpGetIncomingPasswordHelper( const char* passwordData, int *passwordDataLength  ))
RAKPEERANDINTERFACESETPRIVATE(CSharpGetOfflinePingResponseHelper( unsigned char *inOutByteArray, unsigned int *outLength ))
RAKPEERANDINTERFACESETPRIVATE(GetBandwidth);

%csmethodmodifiers SLNet::NetworkIDManager::GET_BASE_OBJECT_FROM_ID "protected"

%csmethodmodifiers SLNet::NetworkIDObject::SetNetworkIDManager "protected"

%csmethodmodifiers  DataStructures::ByteQueue::PeekContiguousBytesHelper "private"

%csmethodmodifiers SLNet::RakNetGUID::ToString() const "public override"

%csmethodmodifiers  SLNet::StatisticsToStringHelper "private"

%csmethodmodifiers SLNet::PacketLogger::FormatLineHelper "private"

%csmethodmodifiers DataStructures::List <unsigned short>::GetHelper "private"
%csmethodmodifiers DataStructures::List <unsigned short>::PopHelper "private"

//FileProgressStruct
%csmethodmodifiers FileProgressStruct::SetFirstDataChunk "private"
%csmethodmodifiers FileProgressStruct::SetIriDataChunk "private"
%csmethodmodifiers OnFileStruct::SetFileData "private"

//ConnectionGraph2
%csmethodmodifiers SLNet::ConnectionGraph2::GetConnectionListForRemoteSystemHelper "private"
%csmethodmodifiers SLNet::ConnectionGraph2::GetParticipantListHelper "private"

#ifdef SWIG_ADDITIONAL_AUTOPATCHER
	//AutopatcherServer
	%csmethodmodifiers SLNet::AutopatcherServer::StartThreadsHelper "private"

	%csmethodmodifiers  SLNet::CreatePatchHelper "private"
#endif

#ifdef SWIG_ADDITIONAL_AUTOPATCHER
	%csmethodmodifiers CompressorBase::GetOutputHelper "private";
#endif

//Operators
%csmethodmodifiers operator > "private"
%csmethodmodifiers operator < "private"
%csmethodmodifiers operator != "private"
%csmethodmodifiers operator [] "private"
%csmethodmodifiers operator >= "private"
%csmethodmodifiers operator <= "private"
%csmethodmodifiers operator / "private"
%csmethodmodifiers operator * "private"
%csmethodmodifiers operator -- "private"
%csmethodmodifiers operator ++ "private"
%csmethodmodifiers operator - "private"
%csmethodmodifiers operator + "private"
%csmethodmodifiers operator+(const SLNet::RakString &lhs, const SLNet::RakString &rhs) "public" //The global MafiaNet operator should be public
