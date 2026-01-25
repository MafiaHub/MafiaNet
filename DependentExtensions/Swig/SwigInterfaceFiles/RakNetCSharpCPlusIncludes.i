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

//This is not parsed by swig but inserted into the generated C++ wrapper, these includes
//are needed so the wrapper includes the needed .h filese
//This also includes the typemaps used.
 %{
 /* Includes the header in the wrapper code */
//Defines
#ifdef SWIGWIN
#define _MSC_VER 10000
#define WIN32
#define _WIN32
#define _DEBUG
#define _RAKNET_DLL
#endif
//TypeDefs
typedef int int32_t;
typedef unsigned int uint32_t;
typedef uint32_t DefaultIndexType;
#ifdef SWIGWIN
typedef unsigned int SOCKET;
#endif
//Includes
#include "mafianet/smartptr.h"
#include "mafianet/defines.h"
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/Export.h"
#include "mafianet/SimpleMutex.h"
#include "mafianet/string.h"
#include "mafianet/wstring.h"
#include "mafianet/BitStream.h"
#include "mafianet/DS_List.h"	
#include "mafianet/DS_ByteQueue.h"
#include "mafianet/assert.h"
#include "mafianet/NativeTypes.h"
#include "mafianet/SocketIncludes.h"
#include "mafianet/time.h"
#include "mafianet/Export.h"
#include "mafianet/memoryoverride.h"
#include "mafianet/types.h"
#include "mafianet/socket.h"
#include "mafianet/statistics.h"
#include "mafianet/NetworkIDObject.h"
#include "mafianet/NetworkIDManager.h"
//The below three classes have been removed from interface, if PluginInterface2 is fully exposed again
//or another class needs them uncomment them and the related typemaps
//#include "mafianet/TCPInterface.h"
//#include "mafianet/PacketizedTCP.h"
//#include "mafianet/InternalPacket.h"
#include "mafianet/PluginInterface2.h"
#include "mafianet/peerinterface.h"
#include "mafianet/peer.h"
#include "mafianet/PacketPriority.h"
#include "mafianet/PacketLogger.h"
#include "mafianet/PacketFileLogger.h"
#include "mafianet/NatTypeDetectionClient.h"
#include "mafianet/NatPunchthroughClient.h"
#include "mafianet/Router2.h"
#include "mafianet/UDPProxyClient.h"
#include "mafianet/FullyConnectedMesh2.h"
#include "mafianet/ReadyEvent.h"
//#include "mafianet/TeamBalancer.h"
#include "mafianet/TeamManager.h"
#include "mafianet/NatPunchthroughServer.h"
#include "mafianet/UDPForwarder.h"
#include "mafianet/UDPProxyServer.h"
#include "mafianet/UDPProxyCoordinator.h"
#include "mafianet/NatTypeDetectionServer.h"
#include "mafianet/DS_BPlusTree.h"
#include "mafianet/DS_Table.h"
#include "mafianet/FileListTransferCBInterface.h"//
#include "mafianet/IncrementalReadInterface.h"//
#include "mafianet/FileListNodeContext.h"//
#include "mafianet/FileList.h"//
#include "mafianet/TransportInterface.h"//
#include "mafianet/CommandParserInterface.h"//
#include "mafianet/LogCommandParser.h"//
#include "mafianet/MessageFilter.h"//
#include "mafianet/DirectoryDeltaTransfer.h"//
#include "mafianet/FileListTransfer.h"//
#include "mafianet/ThreadsafePacketLogger.h"//
#include "mafianet/PacketConsoleLogger.h"//
#include "mafianet/PacketFileLogger.h"//
#include "mafianet/DS_Multilist.h"
#include "mafianet/ConnectionGraph2.h"
#include "mafianet/GetTime.h"
//#include "mafianet/transport2.h"
//#include "mafianet/RoomsPlugin.h"
//Macros
//Swig C++ code only TypeDefs
//Most of these are nested structs/classes that swig needs to understand as global
//They will reference the nested struct/class while appearing global
typedef SLNet::RakString::SharedString SharedString;
typedef DataStructures::Table::Row Row;
typedef DataStructures::Table::Cell Cell; 
typedef DataStructures::Table::FilterQuery FilterQuery;
typedef DataStructures::Table::ColumnDescriptor ColumnDescriptor;
typedef DataStructures::Table::SortQuery SortQuery;
typedef SLNet::FileListTransferCBInterface::OnFileStruct OnFileStruct;
typedef SLNet::FileListTransferCBInterface::FileProgressStruct FileProgressStruct;
typedef SLNet::FileListTransferCBInterface::DownloadCompleteStruct DownloadCompleteStruct;

 %}

#ifdef SWIG_ADDITIONAL_SQL_LITE
	%{
	#include "SQLite3PluginCommon.h"
	#include "SQLite3ClientPlugin.h"
	#include "SQLiteLoggerCommon.h"
	#include "SQLiteClientLoggerPlugin.h"
	#ifdef SWIG_ADDITIONAL_SQL_LITE_SERVER
		#include "SQLite3ServerPlugin.h"
		#include "SQLiteServerLoggerPlugin.h"
	#endif
	typedef SLNet::LogParameter::DataUnion DataUnion;
	typedef SLNet::SQLiteClientLoggerPlugin::ParameterListHelper ParameterListHelper;
	%}
#endif

#ifdef SWIG_ADDITIONAL_AUTOPATCHER
	%{
	#include "mafianet/AutopatcherRepositoryInterface.h"
	#include "AutopatcherServer.h"
	#include "AutopatcherClient.h"
	#include "AutopatcherMySQLRepository.h"
	#include "CreatePatch.h"
	#include "MemoryCompressor.h"
	#include "ApplyPatch.h"
	#include "mafianet/AutopatcherPatchContext.h"
	%}
#endif

#ifdef RAKNET_COMPATIBILITY
%{
using namespace RakNet;
%}
#else
%{
using namespace SLNet;
%}
#endif

