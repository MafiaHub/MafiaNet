Migrating to std:: Containers
=============================

This release removes the ``DataStructures`` containers and the custom
allocator layer that MafiaNet inherited from RakNet. The library now uses the
C++17 standard library (``std::vector``, ``std::deque``, ``std::map``,
``std::set``, ``std::unordered_map``), plain ``new`` / ``delete``,
``std::malloc`` / ``std::free`` and smart pointers. This page lists what was
removed, what replaces it, every public signature that changed, and the
behaviour changes to check when you upgrade.

**What does not change:**

* The wire format. Every message, the ACK/NAK range encoding, the
  ``StringCompressor`` / Huffman output and the ``TableSerializer`` output are
  byte-identical. Old and new peers interoperate.
* ``Packet::data`` stays a raw buffer, released with ``DeallocatePacket()``.
* ``GetInstance()`` / ``DestroyInstance()`` stay the way to create and destroy
  library objects.

**What you must do:** rebuild against the new headers (the ABI changed), then
fix the compile errors in code that names a ``DataStructures`` type, an
allocator macro or one of the signatures below. The tables in
`Mapping the old types`_ give the replacement for each.

Public headers also no longer include ``memory_override.h`` or the ``ds_*.h``
headers, which used to pull in ``<cstring>``, ``<cstdlib>``, ``<new>``,
``<cassert>`` and others transitively. Include the standard headers your own
code uses (``memcpy``, ``strlen``, ``std::malloc``, placement ``new``,
``assert``), or it may stop compiling, especially with GCC/libstdc++.

.. contents:: On this page
   :local:
   :depth: 1

Mapping the old types
---------------------

Containers
~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - DS type / API
     - Replacement
   * - ``List<T>``
     - ``std::vector<T>``
   * - ``List::Push(x, _FILE_AND_LINE_)`` / ``Insert(x, _FILE_AND_LINE_)``
     - ``push_back(x)``
   * - ``List::Insert(x, i, ...)``
     - ``v.insert(v.begin() + i, x)``
   * - ``List::RemoveAtIndex(i)``
     - ``v.erase(v.begin() + i)``
   * - ``List::RemoveAtIndexFast(i)`` (order not kept)
     - ``SwapRemove(v, i)`` from ``vector_util.h``
   * - ``List::GetIndexOf(x)`` (``MAX_UNSIGNED_LONG`` sentinel)
     - ``std::find``; compare against ``end()``
   * - ``List::Clear(true/false, ...)``
     - ``clear()``
   * - ``List::Preallocate(n, ...)``
     - ``reserve(n)``
   * - ``List::Pop()`` / ``Get(i)`` / ``Size()``
     - ``back()`` + ``pop_back()`` / ``[i]`` / ``size()``
   * - ``Queue<T>``
     - ``std::deque<T>``; ``PushAtHead`` → ``push_front``; ``Pop`` →
       ``front()`` + ``pop_front()``; ``operator[]`` kept
   * - ``OrderedList<K,D,cmp>``
     - ``std::set<K>`` when ``D == K``; ``std::map<K, D>`` when not
       index-iterated across calls; otherwise a sorted ``std::vector<D>`` +
       ``std::lower_bound`` with a comparator converted from the 3-way ``cmp``
   * - ``Map<K,V>``
     - ``std::map<K,V>``
   * - ``Hash<K,V>`` / ``HashIndex``
     - ``std::unordered_map<K,V>``; a returned ``HashIndex`` becomes a pointer
       to the mapped value (pointers into ``unordered_map`` survive rehash)
   * - ``Heap<W,D,max>``
     - ``std::vector<std::pair<W,D>>`` + ``std::push_heap`` /
       ``std::pop_heap``, tie-break on an insertion sequence number
   * - ``LinkedList`` / ``CircularLinkedList``
     - ``std::list`` only if an iterator is held across inserts; otherwise
       ``std::vector``
   * - ``Multilist`` / ``MLKeyRef``
     - ``std::vector`` + ``std::find_if`` / ``std::sort``
   * - ``MemoryPool<T>``
     - plain ``new`` / ``delete`` or ``std::unique_ptr``
   * - ``ThreadsafeAllocatingQueue<T>`` / ``SingleProducerConsumer<T>``
     - ``LockedQueue<std::unique_ptr<T>>``
   * - ``ByteQueue``
     - ``std::vector<unsigned char>`` + consumed-prefix ``erase``
   * - ``RangeList<T>``
     - ``MafiaNet::RangeList<T>`` in ``range_list.h``, on ``std::vector``
   * - ``Table`` + ``BPlusTree``
     - ``MafiaNet::Table`` in ``table.h``
   * - ``RakNetSmartPtr<T>``
     - ``std::shared_ptr<T>``
   * - ``_FILE_AND_LINE_`` argument
     - dropped together with the call it was passed to

Allocators
~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - From
     - To
   * - ``MafiaNet::OP_NEW<T>(_FILE_AND_LINE_)``
     - ``new T``
   * - ``MafiaNet::OP_NEW_n<T>(_FILE_AND_LINE_, a1..an)``
     - ``new T(a1..an)``
   * - ``MafiaNet::OP_NEW_ARRAY<T>(n, _FILE_AND_LINE_)``
     - ``new T[n]``
   * - ``MafiaNet::OP_DELETE(p, _FILE_AND_LINE_)`` / ``OP_DELETE_ARRAY``
     - ``delete p`` / ``delete[] p``
   * - ``rakMalloc_Ex(n, _FILE_AND_LINE_)`` / ``rakRealloc_Ex(p, n, ...)`` /
       ``rakFree_Ex(p, ...)``
     - ``std::malloc(n)`` / ``std::realloc(p, n)`` / ``std::free(p)``
   * - ``notifyOutOfMemory(_FILE_AND_LINE_)``
     - removed; keep the surrounding failure return
   * - ``alloca(n)``
     - ``std::vector<T>`` / ``std::string``

One difference to know when porting your own code: ``OP_NEW_ARRAY<T>(0)``
returned ``nullptr``, while ``new T[0]`` returns a non-null pointer that must
still be ``delete[]``-ed.

Porting examples
~~~~~~~~~~~~~~~~

.. code-block:: cpp

   // Before
   DataStructures::List<MafiaNet::SystemAddress> addresses;
   DataStructures::List<MafiaNet::RakNetGUID> guids;
   peer->GetSystemList(addresses, guids);
   for (unsigned i = 0; i < addresses.Size(); i++)
       Use(addresses[i]);
   if (guids.GetIndexOf(target) != MAX_UNSIGNED_LONG)
       ...

   // After
   std::vector<MafiaNet::SystemAddress> addresses;
   std::vector<MafiaNet::RakNetGUID> guids;
   peer->GetSystemList(addresses, guids);
   for (const MafiaNet::SystemAddress &address : addresses)
       Use(address);
   if (std::find(guids.begin(), guids.end(), target) != guids.end())
       ...

.. code-block:: cpp

   // Before
   unsigned char *copy;
   BitSize_t bits = bitStream.CopyData(&copy);
   ...
   rakFree_Ex(copy, _FILE_AND_LINE_);

   // After: the buffer frees itself
   std::unique_ptr<unsigned char[]> copy;
   BitSize_t bits = bitStream.CopyData(copy);

Out-parameters keep their shape: a function that filled a
``DataStructures::List<T>&`` now fills a ``std::vector<T>&``, and clears it
first wherever it cleared it before. In-parameters that were taken by value
are now ``const std::vector<T>&``.

Removed headers, macros and functions
-------------------------------------

**Headers.** None of these has a shim; include the standard header instead.

* Every ``ds_*.h`` header: ``ds_binary_search_tree.h``, ``ds_bplus_tree.h``,
  ``ds_byte_pool.h``, ``ds_byte_queue.h``, ``ds_hash.h``, ``ds_heap.h``,
  ``ds_huffman_encoding_tree_factory.h``, ``ds_linked_list.h``,
  ``ds_list.h``, ``ds_map.h``, ``ds_memory_pool.h``, ``ds_multilist.h``,
  ``ds_ordered_channel_heap.h``, ``ds_ordered_list.h``, ``ds_queue.h``,
  ``ds_queue_linked_list.h``, ``ds_range_list.h``, ``ds_table.h``,
  ``ds_threadsafe_allocating_queue.h``, ``ds_tree.h``,
  ``ds_weighted_graph.h``.
* ``ds_huffman_encoding_tree.h`` and ``ds_huffman_encoding_tree_node.h`` are
  renamed ``huffman_encoding_tree.h`` and ``huffman_encoding_tree_node.h``.
  ``ds_table.h`` is replaced by ``table.h`` and ``ds_range_list.h`` by
  ``range_list.h``.
* ``memory_override.h``, ``alloca.h``, ``smart_ptr.h`` (``RakNetSmartPtr``)
  and ``single_producer_consumer.h``.

**Namespace.** ``DataStructures`` no longer exists. The types that survive
(``RangeList``, ``RangeNode``, ``Table``, ``HuffmanEncodingTree``,
``HuffmanEncodingTreeNode``) are in ``MafiaNet``.

**Macros and constants:** ``_FILE_AND_LINE_``, ``_USE_RAK_MEMORY_OVERRIDE``,
``USE_ALLOCA``, ``MAX_ALLOCA_STACK_ALLOCATION``,
``INTERNAL_PACKET_PAGE_SIZE`` and ``BUFFERED_PACKETS_PAGE_SIZE``
(``defines.h``); ``RESEND_TREE_ORDER`` (``reliability_layer.h``);
``_TABLE_BPLUS_TREE_ORDER`` (``ds_table.h``); ``MAX_UNSIGNED_LONG``
(``ds_list.h``, the ``GetIndexOf`` not-found sentinel); and the
``_CSTRING_COMPRESSOR`` build switch together with the ``CString`` overloads
of ``StringCompressor``. Code that uses one of them no longer compiles;
defining one of the build switches has no effect.

**Allocator functions:** ``SetMalloc`` / ``SetRealloc`` / ``SetFree`` and
their ``_Ex`` variants, ``SetNotifyOutOfMemory``, ``GetMalloc`` /
``GetRealloc`` / ``GetFree``, ``rakMalloc`` / ``rakRealloc`` / ``rakFree``
(and ``_Ex``), ``OP_NEW`` / ``OP_NEW_n`` / ``OP_NEW_ARRAY`` / ``OP_DELETE`` /
``OP_DELETE_ARRAY`` and ``notifyOutOfMemory``. The bundled dlmalloc
(``UseRaknetFixedHeap``) went with them.

**Types and comparators:** ``RakNetSmartPtr``, ``SplitPacketChannelComp``
(``reliability_layer.h``),
``ConnectionGraph2::RemoteSystemComp`` and ``SystemAddressAndGuidComp``,
``RPC::LocalCallbackComp``,
``VariableDeltaSerializer::UpdatedVariablesListPtrComp`` and
``RoomsPluginParticipantCompByRakString``. They only served the removed
containers.

**New helpers** (all in ``MafiaNet``):

* ``vector_util.h``: ``SwapRemove(v, i)``, the O(1) unordered erase that
  replaces ``RemoveAtIndexFast``.
* ``instance_ptr.h``: ``InstancePtr<T>`` / ``MakeInstance<T>()``, a
  ``unique_ptr`` that releases through ``T::DestroyInstance``.
* ``locked_queue.h``: ``LockedQueue<T>``, a mutex-guarded ``std::deque`` with
  ``Push``, ``std::optional<T> TryPop()``, ``IsEmpty``, ``Size`` and
  ``Clear``.
* ``range_list.h`` and ``table.h``: the re-homed ``RangeList`` and ``Table``.
* ``object_pool.h``: ``ObjectPool<T>`` exists only to recycle
  ``ReliabilityLayer``'s ``InternalPacket`` objects. It is an internal
  detail, not a general-purpose API, and may change without notice.

Allocation, ownership and the DLL boundary
------------------------------------------

**Replacing the allocator.** There is no library-private allocator hook any
more. To track, pool or control MafiaNet's allocations, replace the allocator
for the whole process: replace the global ``operator new`` / ``operator
delete`` (all forms, including array and sized ones), or link a drop-in
``malloc`` such as mimalloc or jemalloc. See
:doc:`custom-memory-management` for an example.

**Owned buffers are smart pointers.** A function that hands you a heap buffer
now fills a ``std::unique_ptr<unsigned char[]>``:

* ``BitStream::CopyData(std::unique_ptr<unsigned char[]> &data) const``
  (was ``unsigned char **``). An empty stream leaves ``data`` null and returns
  0.
* ``BitStream::ReadAlignedBytesSafeAlloc(std::unique_ptr<unsigned char[]>
  &out, int &length, unsigned maxBytesToRead)`` and the ``unsigned int &``
  overload (were ``char **``).
* ``DataCompressor::DecompressAndAllocate(BitStream *input,
  std::unique_ptr<unsigned char[]> &output)`` (was ``unsigned char **``). It
  now rejects a declared size larger than the stream can encode.

**Factories.** ``GetInstance()`` / ``DestroyInstance()`` stay, because memory
must be freed by the module that allocated it. Hold a factory object in
``InstancePtr<T>`` (``instance_ptr.h``), never in a
``std::unique_ptr<T>`` with the default deleter. ``STATIC_FACTORY_DEFINITIONS``
now expands to ``new`` / ``delete``.

**Shared buffers.** ``RakNetSmartPtr<T>`` is ``std::shared_ptr<T>``. In
Lobby2, ``BinaryDataBlock`` fields are ``std::shared_ptr`` and are created
with ``std::make_shared``; test them against ``nullptr`` instead of
``IsNull()``. The buffers those blocks own,
``BinaryDataBlock::binaryData`` and ``IndexedBinaryValue::value``, are freed
with ``std::free`` (they were ``rakFree_Ex``): allocate what you hand them
with ``std::malloc``.

Windows: CRT and toolset
~~~~~~~~~~~~~~~~~~~~~~~~

``std::`` types (``std::vector``, ``std::map``, ``std::string``,
``std::unique_ptr``, ...) now appear in the public headers and cross the DLL
boundary when you link ``MafiaNet.dll``. The standard library has no stable
ABI across compiler versions, CRTs or build configurations, so **the
application and MafiaNet.dll must be built with the same compiler toolset and
the same CRT**: same MSVC major toolset, same ``/MD`` vs ``/MT``, and Debug
with Debug (``/MDd``), Release with Release (``/MD``). A Release DLL used by a
Debug application, or a DLL built with a different toolset, corrupts the heap
or crashes when a container crosses the boundary. If you cannot guarantee
this, link the static library instead. Linux and macOS builds have the same
rule in principle (one standard library and one ABI per process), which the
platform toolchains normally ensure.

Changed public signatures
-------------------------

Every out-parameter that was a ``DataStructures::List<T>&`` is now a
``std::vector<T>&`` with the same meaning. The lists below cover every
declaration that changed in the installed headers (``Source/include/mafianet``)
and in the ``DependentExtensions`` headers, grouped by subsystem. Functions
are given with their new signature. Where only the container type of data
members changed, one line names those members. Protected members are listed
because subclasses and plugins see them.

RakPeer and sockets
~~~~~~~~~~~~~~~~~~~

* ``RakPeerInterface::GetSystemList(std::vector<SystemAddress>&,
  std::vector<RakNetGUID>&) const``
* ``RakPeerInterface::GetSockets(std::vector<RakNetSocket2*>&)`` and
  ``ReleaseSockets(std::vector<RakNetSocket2*>&)``
* ``RakPeerInterface::GetStatisticsList(std::vector<SystemAddress>&,
  std::vector<RakNetGUID>&, std::vector<RakNetStatistics>&)``
* ``RakPeer`` (protected): ``AllocPacket(unsigned dataSize)`` and
  ``AllocPacket(unsigned dataSize, unsigned char *data)`` lost their
  ``const char *file, unsigned int line`` parameters, as did
  ``AllocRNS2RecvStruct()`` and ``DeallocRNS2RecvStruct(RNS2RecvStruct*)``.
  ``CallPluginCallbacks`` takes a ``std::vector<PluginInterface2*>&``.
* ``RakPeer`` members ``pluginListTS``, ``pluginListNTS`` and
  ``pluginListNetworkThread`` are ``std::vector<PluginInterface2*>``;
  ``banList`` and ``socketList`` are
  ``std::vector``; ``requestedConnectionQueue``, ``bufferedPacketsQueue`` and
  ``packetReturnQueue`` are ``std::deque``; ``bufferedCommands`` and
  ``socketQueryOutput`` are ``LockedQueue<std::unique_ptr<...>>``. The
  ``packetAllocationPool`` and ``remoteSystemIndexPool`` memory pools are
  gone. ``BufferedCommandStruct`` owns its ``data`` and is non-copyable.
  ``RemoteSystemStruct::connectMode`` is ``std::atomic<ConnectMode>``.
* ``RNS2EventHandler::AllocRNS2RecvStruct()`` and
  ``DeallocRNS2RecvStruct(RNS2RecvStruct*)`` (pure virtuals in
  ``socket2.h``): the ``file, line`` parameters are gone. Override them
  without those parameters.
* ``RakNetSocket2::Send``, ``SendBatch``, ``RNS2_Berkley::Bind``,
  ``BindShared``, ``BindSharedIPV4And6``,
  ``RNS2_Windows_Linux::Send_Windows_Linux`` and the ``RNS2_Windows`` /
  ``RNS2_Linux`` overrides: the ``file, line`` parameters are gone.

ReliabilityLayer
~~~~~~~~~~~~~~~~

``reliability_layer.h`` is installed but is internal in practice. Changes:

* ``SplitPacketChannelComp`` (a ``RAK_DLL_EXPORT`` function),
  ``SortSplitPacketList`` (declared, never defined) and ``RESEND_TREE_ORDER``
  are removed.
* ``BPSTracker::Reset()`` takes no arguments (was ``Reset(const char *file,
  unsigned int line)``).
* ``HandleSocketReceiveFromConnectedPlayer``, ``Update``,
  ``UpdateAndForceACKs``, ``UpdateInternal`` and
  ``RemovePacketFromResendListAndDeleteOlderReliableSequenced`` take
  ``std::vector<PluginInterface2*>&``.
* ``AllocInternalPacketData(InternalPacket*, unsigned numBytes, bool
  allowStack)`` and ``FreeInternalPacketData(InternalPacket*)`` lost their
  ``file, line`` parameters.
* The send and ordering heaps are ``std::vector<HeapEntry>`` driven by
  ``HeapPush`` / ``HeapPop``. ``HeapEntry``, ``HeapEntryPopsLater``,
  ``HeapPush`` and ``HeapPop`` are public only so the unit tests can reach
  them; they are marked ``\internal``. The other member containers are
  ``std::deque``, ``std::vector`` and ``std::map``.

BitStream, compression and strings
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``BitStream::CopyData``, ``ReadAlignedBytesSafeAlloc`` and
  ``DataCompressor::DecompressAndAllocate``: see
  `Allocation, ownership and the DLL boundary`_.
* ``HuffmanEncodingTree`` / ``HuffmanEncodingTreeNode`` moved to
  ``MafiaNet`` and to ``huffman_encoding_tree.h`` /
  ``huffman_encoding_tree_node.h``. ``HuffmanEncodingTreeNode::weight`` is
  ``uint64_t``.
  ``size_t DecodeArray(BitStream*, BitSize_t, size_t maxCharsToWrite,
  unsigned char*)`` returns ``size_t`` (was ``unsigned``): the number of
  symbols decoded, which may exceed ``maxCharsToWrite`` (only that many are
  written), or the new ``HuffmanEncodingTree::DECODE_ERROR``.
  ``bool DecodeArray(unsigned char*, BitSize_t, BitStream*)`` returns
  ``false`` when there is no tree (was ``void``). The tree is non-copyable.
* ``StringCompressor`` stores its trees in
  ``std::map<int, std::unique_ptr<HuffmanEncodingTree>>`` and is
  non-copyable. The ``_CSTRING_COMPRESSOR`` overloads are removed.
* ``StringTable`` keeps its strings in a sorted ``std::vector`` and is
  non-copyable.
* Protocol 11 uses ``std::string`` for narrow strings and removes the custom free list.
* ``std::hash<RakNetGUID>`` (``types.h``) allows GUIDs to key a ``std::unordered_map``.
  ``std::hash<AddressOrGUID>`` is intentionally **not** provided:
  ``AddressOrGUID::operator==`` matches on the GUID *or* the address, so no
  hash can be consistent with it. Key on ``RakNetGUID`` or ``SystemAddress``
  instead.
* ``RangeList<T>`` / ``RangeNode<T>`` are in ``MafiaNet`` (``range_list.h``):
  ``ranges`` is a ``std::vector<RangeNode<T>>``, and the ``RangeNodeComp``
  comparator and the constructor/destructor are gone.

RPC
~~~

* ``RPC4::GetLocalSlotIndex(const char*)`` (protected, returned a
  ``HashIndex``) is replaced by public ``LocalSlot *GetLocalSlot(const char
  *sharedIdentifier)``, which returns ``nullptr`` for an unknown name. The
  pointer is owned by RPC and stays valid until ``UnregisterSlot()`` for that
  identifier or RPC's destruction; more ``RegisterSlot()`` calls with the
  same identifier add to the same slot.
* ``RPC::InvokeSignal(LocalSlot *slot, BitStream*, Packet*)`` is public and
  takes that handle (was a protected ``HashIndex`` overload). ``nullptr`` is a
  no-op.
* ``RPC::LocalSlot`` is only forward-declared in the header. ``localSlots``
  is now private and is a
  ``std::unordered_map<std::string, std::unique_ptr<LocalSlot>>``;
  ``registeredNonblockingFunctions`` / ``registeredBlockingFunctions`` are
  ``std::unordered_map``; ``localCallbacks`` is a
  ``std::map<MessageID, LocalCallback>``. ``LocalCallbackComp`` is removed.
  RPC is non-copyable.

.. code-block:: cpp

   // Before (inside an RPC4 subclass)
   DataStructures::HashIndex index = GetLocalSlotIndex("OnHit");
   if (index.IsInvalid() == false)
       InvokeSignal(index, &params, packet);

   // After (public API)
   if (MafiaNet::RPC::LocalSlot *slot = rpc->GetLocalSlot("OnHit"))
       rpc->InvokeSignal(slot, &params, packet);

ReplicaManager3
~~~~~~~~~~~~~~~

All take ``std::vector`` where they took ``DataStructures::List``:

* ``ReplicaManager3``: ``AutoCreateConnectionList(std::vector<RakNetGUID>&,
  std::vector<Connection_RM3*>&, ...)``, ``DereferenceList``,
  ``GetReplicasCreatedByGuid``, ``GetReplicasCreatedByMe``,
  ``GetReferencedReplicaList``, ``GetConnectionsInVirtualWorld``,
  ``GetGuidsInVirtualWorld``, ``GetConnectionsThatHaveReplicaConstructed``,
  ``BroadcastDestructionList``.
* ``Connection_RM3`` virtuals (every override must change):
  ``GetConstructedReplicas(std::vector<Replica3*>&)``,
  ``QueryReplicaList(std::vector<Replica3*>&, std::vector<Replica3*>&)``,
  ``QuerySerializationList(std::vector<Replica3*>&)``,
  ``SendConstruction(std::vector<Replica3*>&, std::vector<Replica3*>&,
  ...)``.
* Internal lists are ``std::vector``; ``downloadGroup`` is
  ``std::deque<Packet*>``; the constructed-replica list is a sorted
  ``std::vector<LastSerializationResult*>`` (same order as before).

FullyConnectedMesh2 and ConnectionGraph2
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``FullyConnectedMesh2::GetHostOrder(std::vector<RakNetGUID>&)`` and
  ``GetParticipantList(std::vector<RakNetGUID>&)``.
* Virtuals ``GetVerifiedJoinAcceptedAdditionalData(..., std::vector<RakNetGUID>&,
  ...)`` and ``GetVerifiedJoinRequiredProcessingList(...,
  std::vector<SystemAddress>&, std::vector<RakNetGUID>&,
  std::vector<BitStream*>&)``; protected ``CategorizeVJIP`` takes five
  ``std::vector<RakNetGUID>&``.
* ``ConnectionGraph2::GetParticipantList(std::set<RakNetGUID>&)`` (was an
  ``OrderedList<RakNetGUID,RakNetGUID>&``; same RakNetGUID order). Its
  internal maps own their entries with ``std::unique_ptr``; the
  ``RemoteSystemComp`` and ``SystemAddressAndGuidComp`` statics are removed.
  ConnectionGraph2 is non-copyable.

ReadyEvent, Router2, RakVoice and sectorizers
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``ReadyEvent``: the event and wait lists are sorted ``std::vector`` (the
  ``GetEventAtIndex`` / ``GetFromWaitListAtIndex`` order is unchanged). New
  protected helpers ``FindEventIndex(int, bool*) const`` and static
  ``FindSystemIndex(const std::vector<RemoteSystem>&, RakNetGUID, bool*)``.
* ``Router2``: internal lists are ``std::vector``; no public signature
  changed.
* ``RakVoice::GetActiveSpeakers(std::vector<RakNetGUID>&)``; new protected
  ``FindVoiceChannel`` / ``HasVoiceChannel``.
* ``GridSectorizer::GetEntries(std::vector<void*>&, ...)`` and
  ``PointGridSectorizer::GetEntries(std::vector<void*>&, ...) const``.

MessageFilter, RelayPlugin, StatisticsHistory
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``MessageFilter`` (protected members): ``FilterSet::allowedRPC`` is a
  ``std::set<std::string>``, ``filterList`` an id-sorted
  ``std::vector<std::unique_ptr<FilterSet>>``, ``systemList`` a
  ``std::unordered_map<AddressOrGUID, FilteredSystem,
  MessageFilterSystemHash>``. Non-copyable.
* ``RelayPlugin`` (protected members): ``std::unordered_map`` and
  ``std::vector`` with ``unique_ptr`` ownership; new protected
  ``FindParticipant(const RakNetGUID&)``. Non-copyable.
* ``StatisticsHistory``: ``GetHistorySorted(uint64_t, SHSortOperation,
  std::vector<TimeAndValueQueue*>&) const`` (still appends),
  ``GetUniqueKeyList(std::vector<std::string>&)`` (clears, then fills),
  ``TimeAndValueQueue::values`` is a ``std::deque<TimeAndValue>`` and
  ``ResizeSampleSet`` takes a ``std::deque<TimeAndValue>&``. Protected:
  ``TrackedObject::dataQueues`` and ``objects`` own their entries with
  ``std::unique_ptr``; new ``FindObject``. ``StatisticsHistory`` and
  ``StatisticsHistoryPlugin`` are non-copyable.

Command parsers, SQL parser and delta serializers
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``VariadicSQLParser::GetTypeMappingIndices(..., std::vector<IndexAndType>&)``,
  ``ExtractArguments(..., const std::vector<IndexAndType>&, ...)``,
  ``FreeArguments(const std::vector<IndexAndType>&, ...)``.
* Protected members on ``std::`` containers: ``CommandParserInterface::commandList``
  (sorted ``std::vector``), ``ConsoleServer::commandParserList``,
  ``LogCommandParser::remoteUsers``,
  ``VariableDeltaSerializer::updatedVariablesHistory`` (``std::map``) and
  ``remoteSystemVariableHistoryList``,
  ``VariableListDeltaTracker::variableList``.

File transfer
~~~~~~~~~~~~~

* ``FileList::fileList`` (public) is a ``std::vector<FileListNode>``;
  ``FileList::GetCallbacks`` and ``FileListTransfer::GetCallbacks`` take
  ``std::vector<FileListProgress*>&``.
* ``FileListTransfer`` (protected): ``fileListReceivers`` is a
  ``std::map``, ``FileToPushRecipient::filesToPush`` a ``std::deque``, the
  other lists ``std::vector``.

TCP, HTTP and transports
~~~~~~~~~~~~~~~~~~~~~~~~

* ``TCPInterface``: ``messageHandlerList`` is
  ``std::vector``; ``headPush`` / ``tailPush`` ``std::deque``; the incoming
  and connection-attempt queues ``LockedQueue`` (their ``SimpleMutex``
  members are gone); ``RemoteClient::outgoingData`` is a
  ``std::vector<unsigned char>`` consumed from ``outgoingOffset`` (helpers
  ``TcpOutgoingPending`` / ``TcpOutgoingConsume``). The never-used
  ``requestedCloseConnections`` and ``newRemoteClients`` members are removed.
  Non-copyable.
* ``PacketizedTCP``: ``connections`` is a
  ``std::map<SystemAddress, std::vector<unsigned char>>``, the queues
  ``std::deque``. New: ``static constexpr uint32_t
  DEFAULT_MAX_MESSAGE_LENGTH`` (64 MiB), ``SetMaxMessageLength(uint32_t)``,
  ``GetMaxMessageLength() const`` and protected ``OnBytesReceived(...)``. See
  `Behaviour changes`_. Non-copyable.
* ``HTTPConnection::outgoingCommand`` (public), ``badResponses`` and
  ``results`` are ``std::deque``; ``HTTPConnection2``'s request lists are
  ``std::deque`` / ``std::vector``.
* ``TelnetTransport::remoteClients`` is a ``std::vector``;
  ``RakNetTransport2``'s queues are ``std::deque``.

Threads and utilities
~~~~~~~~~~~~~~~~~~~~~

* ``ThreadPool``: one ``std::deque<std::pair<WorkerCallback, InputType>>
  inputQueue`` replaces the paired input queues; ``outputQueue`` is a
  ``std::deque``; the two queue mutexes are ``std::mutex``. ``GetOutput()``
  on an empty queue returns a value-initialized ``OutputType``.
  Non-copyable.

  The queue mutexes are **not recursive**. On Windows they used to be a
  ``CRITICAL_SECTION``, which the owning thread could take again, so code
  that called ``AddInput()`` (or anything else that locks the input queue)
  while holding ``LockInput()`` worked there and deadlocked elsewhere. It now
  deadlocks on every platform: release the lock first.
* ``CCRakNetUDT::pingsLastInterval`` and ``RakNetRandomSync::usedValues`` are
  ``std::deque``.
* ``UDPForwarder``: ``startForwardingInput`` / ``stopForwardingCommands`` are
  ``LockedQueue``, ``startForwardingOutput`` a ``std::deque``,
  ``forwardListNotUpdated`` a ``std::vector``.
* ``SendToThread::objectQueue`` is a
  ``LockedQueue<std::unique_ptr<SendToThreadBlock>>``;
  ``ThreadsafePacketLogger::logMessages`` a ``LockedQueue<std::string>``.

NAT traversal and UDP proxy
~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``NatPunchthroughClient``: public ``failedAttemptList`` is a
  ``std::vector``, ``queuedOpenNat`` a ``std::deque``. ``OpenNATGroup`` (still
  compiled out) takes ``const std::vector<RakNetGUID>&``.
* ``NatPunchthroughServer``: ``users`` is a ``std::map<RakNetGUID, User*>``;
  ``User::groupPunchthroughRequests`` a ``std::set``,
  ``User::connectionAttempts`` a ``std::vector``.
* ``NatTypeDetectionClient`` / ``NatTypeDetectionServer``: the buffered
  packets are a ``LockedQueue<std::unique_ptr<RNS2RecvStruct>>`` (the mutex is
  gone), ``natDetectionAttempts`` a ``std::vector``; the
  ``AllocRNS2RecvStruct()`` / ``DeallocRNS2RecvStruct()`` overrides lost
  ``file, line``. Both are non-copyable.
* ``UDPProxyClient``: ``PingServerGroup::serversToPing`` and
  ``pingServerGroups`` are ``std::vector``.
* ``UDPProxyCoordinator``: ``ForwardingRequest::remainingServersToTry`` is a
  ``std::deque``, ``sourceServerPings`` / ``targetServerPings`` and
  ``serverList`` ``std::vector``, ``forwardingRequestList`` a sorted
  ``std::vector``.
* ``UDPProxyServer``: ``loggingInCoordinators`` / ``loggedInCoordinators``
  are ``std::set<SystemAddress>``.

Table and TableSerializer
~~~~~~~~~~~~~~~~~~~~~~~~~

* ``DataStructures::Table`` (``ds_table.h``) is ``MafiaNet::Table``
  (``table.h``).
* ``GetRows()`` returns ``const std::map<unsigned,
  std::unique_ptr<Row>>&`` (``Table::RowMap``, ascending row id).
  ``GetListHead()`` is removed: iterate ``GetRows()``.

  .. code-block:: cpp

     // Before
     DataStructures::Page<unsigned, Table::Row *, _TABLE_BPLUS_TREE_ORDER> *cur = table.GetListHead();
     while (cur) { for (int i = 0; i < cur->size; i++) Use(cur->keys[i], cur->data[i]); cur = cur->next; }

     // After
     for (const auto &[rowId, row] : table.GetRows())
         Use(rowId, row.get());

* ``GetColumns()`` returns ``const std::vector<ColumnDescriptor>&``.
* ``Row::cells`` is ``std::vector<std::unique_ptr<Cell>>``. ``Table::Row`` is
  non-copyable; copy a row with ``AddRow(rowId, row->cells, true)``.
* ``AddRow(unsigned, const std::vector<Cell>&)``,
  ``AddRow(unsigned, const std::vector<Cell*>&, bool copyCells = false)`` and
  the new ``AddRow(unsigned, const std::vector<std::unique_ptr<Cell>>&, bool
  copyCells = false)``. Every overload returns ``nullptr`` for an existing
  row id.
* Protected ``AddRowColumns`` and ``QueryRow`` take
  ``const std::vector<unsigned>&``; ``DeleteRow`` is removed.
* ``Table`` keeps a deep copy, now as both a copy constructor and
  ``operator=``.
* ``FilterQuery``, ``Cell`` and ``ColumnDescriptor`` fields have default
  initializers.
* ``TableSerializer``: every signature takes ``MafiaNet::Table``;
  ``SerializeColumns(..., const std::vector<int>&)`` and both
  ``SerializeRow`` overloads take ``const std::vector<ColumnDescriptor>&``
  (and ``const std::vector<int>&`` for the skip list).

DependentExtensions
~~~~~~~~~~~~~~~~~~~

Every header under ``DependentExtensions/`` whose declarations changed is
listed below, by extension. ``RakNetSmartPtr<BinaryDataBlock>`` became
``std::shared_ptr<BinaryDataBlock>`` everywhere it appeared.

**Lobby2** (historical; the extension has since been removed)

* ``Lobby2Client``: ``virtual void SetIgnoreList(const std::vector<RakString>
  &users)`` (was ``DataStructures::List<RakString>`` by value);
  ``const std::set<RakString> *GetIgnoreList() const`` (was
  ``const OrderedList<RakString,RakString>*``); the protected ``ignoreList``
  member is a ``std::set<RakString>``.
* ``Lobby2Server``:

  * ``void SendMsg(Lobby2Message*, const std::vector<SystemAddress>
    &recipients)``
  * ``bool HasAdminAddress(const std::vector<SystemAddress>&)`` and
    ``bool HasRankingAddress(const std::vector<SystemAddress>&)``
  * ``void SendRemoteLoginNotification(RakString handle, const
    std::vector<SystemAddress> &recipients)``
  * ``void SendUnifiedToMultiple(const BitStream*, Priority, Reliability, char,
    const std::vector<SystemAddress> &systemAddresses)`` (was by value)
  * ``const std::vector<User*> &GetUsers() const`` (was
    ``const OrderedList<RakString, User*, UserCompByUsername>&``; still sorted
    by user name), and a new protected
    ``unsigned GetUserInsertionIndex(const RakString&, bool *objectExists)
    const``
  * ``Lobby2ServerCommand::callerSystemAddresses`` / ``callerGuids`` and
    ``User::systemAddresses`` / ``guids`` are ``std::vector``; protected
    ``adminAddresses`` / ``rankingAddresses`` are ``std::set<SystemAddress>``,
    ``users`` a sorted ``std::vector<User*>``, ``threadActionQueue`` a
    ``std::deque<ThreadAction>``.

* ``Lobby2Plugin``: protected ``callbacks`` is a
  ``std::vector<Lobby2Callbacks*>``.
* ``Lobby2Message.h``: every message's list member is a ``std::vector`` of the
  same element type (for example ``Friends_GetInvites::invitesSent``,
  ``Emails_Send::recipients``, ``Clans_Get::clans``); every
  ``binaryData`` member is ``std::shared_ptr<BinaryDataBlock>`` created with
  ``std::make_shared``; ``Lobby2Message::ValidateBinary(std::shared_ptr<BinaryDataBlock>)``.
  Wire counts are written with the same widths as before.
* ``Lobby2Message_Steam.h``: the list members are ``std::vector``
  (``Console_SearchRooms_Steam::roomNames`` / ``roomIds``).
* ``Lobby2Client_Steam``: pure virtual ``GetRoomMembers(std::set<uint64_t>&)``
  (was ``OrderedList<uint64_t,uint64_t>&``).
  ``Lobby2Client_Steam_Impl`` overrides it; its members are
  ``std::vector<Lobby2Message*> deferredCallbacks``,
  ``std::map<SystemAddress, RoomMember> roomMembersByAddr``,
  ``std::map<uint64_t, RoomMember> roomMembersById`` and
  ``std::set<uint64_t> rooms``. The ``SystemAddressAndRoomMemberComp`` and
  ``SteamIDAndRoomMemberComp`` statics are removed.
* Rooms, ``RoomsContainer.h``:

  * ``AllGamesRoomsContainer`` (public):
    ``GetInvitesToParticipant(RoomsParticipant*, std::vector<InvitedUser*>&)``,
    ``GrantModerator(RoomsParticipant*, RoomsParticipant*,
    std::vector<InvitedUser>&)``,
    ``SetCustomRoomProperties(RoomsParticipant*, MafiaNet::Table*)``,
    ``GetReadyStatus(RoomID, Room**, std::vector<RoomsParticipant*>&,
    std::vector<RoomsParticipant*>&)``,
    ``GetRoomProperties(RoomID, Room**, MafiaNet::Table*)``,
    ``ProcessQuickJoins(std::vector<QuickJoinUser*>&,
    std::vector<JoinedRoomResult>&, std::vector<QuickJoinUser*>&, ...)`` and
    ``SearchByFilter(GameIdentifier, RoomsParticipant*, RoomQuery*,
    std::vector<Room*>&, bool)`` (was an ``OrderedList`` sorted by name; the
    vector comes back in the same order). ``perGamesRoomsContainers`` is a
    ``std::map``.
  * ``PerGameRoomsContainer`` (protected):
    ``GetRoomNames(std::vector<RakString>&)``,
    ``GetAllRooms(std::vector<Room*>&)``, ``GetInvitesToParticipant``,
    ``ProcessQuickJoins``, ``RoomPrioritySort(RoomsParticipant*, RoomQuery*,
    std::vector<Room*>&)`` and ``SearchByFilter(RoomsParticipant*, RoomQuery*,
    std::vector<Room*>&, bool)``; ``roomsTable`` is a ``MafiaNet::Table``,
    ``quickJoinList`` a ``std::vector``.
  * ``Room``: ``GrantModerator(..., std::vector<InvitedUser>&)``,
    ``SetCustomRoomProperties(..., MafiaNet::Table*)``,
    ``GetReadyStatus(std::vector<RoomsParticipant*>&,
    std::vector<RoomsParticipant*>&)``, ``GetInvitesToParticipant(...,
    std::vector<InvitedUser*>&)``, the constructor takes a
    ``MafiaNet::Table::Row*``, and the static ``UpdateRowSlots`` /
    ``UpdateUsedSlots`` take ``MafiaNet::Table::Row*``. Member lists are
    ``std::vector``.
  * Structs: ``RemoveUserResult::clearedInvitations``,
    ``RoomDescriptor::roomMemberList`` / ``banList`` and
    ``RoomCreationParameters::Validate(const std::vector<RakString>&,
    ProfanityFilter*) const`` use ``std::vector``;
    ``RoomDescriptor::roomProperties`` is a ``MafiaNet::Table`` and
    ``GetProperty()`` returns ``MafiaNet::Table::Cell*``; ``RoomQuery`` uses
    ``MafiaNet::Table::FilterQuery`` / ``FilterQueryType`` /
    ``Cell`` (``AddQuery_NUMERIC`` / ``_STRING`` / ``_BINARY`` /
    ``_POINTER``).
  * New free template ``InsertSortedUnique(std::vector<T>&, const T&, int
    (*compare)(const T&, const T&))``. It is an internal helper that
    reproduces ``OrderedList::Insert`` for comparators that are not strict
    weak orders.

* Rooms, ``RoomTypes.h``: ``DefaultRoomColumns::GetColumnType`` returns
  ``MafiaNet::Table::ColumnType``; ``AddDefaultColumnsToTable`` and
  ``HasDefaultColumns`` take ``MafiaNet::Table*``.
* Rooms, ``RoomsPlugin.h``: the message structs' ``Table`` members are
  ``MafiaNet::Table`` and their lists ``std::vector`` (for example
  ``SetCustomRoomProperties_Func::table``, ``GetReadyStatus_Func::readyUsers``,
  ``SearchByFilter_Func::roomsOutput``); ``roomsCallback`` and
  ``loginServers`` are ``std::vector``; ``roomsParticipants`` is a
  ``std::map<RakString, std::unique_ptr<RoomsPluginParticipant>>`` and
  ``RoomsPluginParticipantCompByRakString`` is removed. ``RoomsPlugin`` is
  non-copyable.
* Rooms, ``ProfanityFilter.h``: ``words`` is a ``std::vector<RakString>``.

Non-copyable types
------------------

These classes now delete their copy constructor and copy assignment. Most
were already unsafe to copy (a copy shared owned pointers and freed them
twice); the deletion makes that a compile error:

``ConnectionGraph2``, ``TCPInterface``, ``PacketizedTCP``, ``ThreadPool``,
``NatTypeDetectionClient``, ``NatTypeDetectionServer``,
``RoomsPlugin``, ``RPC``, ``MessageFilter``, ``RelayPlugin``,
``StatisticsHistory``, ``StatisticsHistoryPlugin``, ``StringCompressor``,
``StringTable``, ``HuffmanEncodingTree``, ``Table::Row`` and the new
``LockedQueue`` and ``ObjectPool``. ``Table`` itself keeps a deep copy.

Behaviour changes
-----------------

* **SortTable keeps tied rows.** ``Table::SortTable`` used to drop a row
  whose sort columns all tied with another (and assert in Debug). Fully tied
  rows are now kept, in ascending row-id order (a stable sort). Results for
  inputs without ties are unchanged.
* **Router2's forwarding choice is deterministic.** The old comparator was
  not a valid ordering, so the router asked depended on insertion order.
  Router2 now picks the lowest weighted ping
  (``ping * (usedForwardingEntries + 1)``, computed in 64 bits: both factors
  come from the relay and the ``int`` product used to overflow), then the
  lowest GUID. Not visible on the wire.
* **Peer-supplied names are matched verbatim.** RPC and
  MessageFilter looked up names received from the
  network through ``RakString``'s printf-style constructor, so the remote
  string was used as a format string. A ``%`` in a name was undefined
  behaviour. Names are now stored and matched as plain strings; only names
  containing ``%`` are affected.
* **RPC::UnregisterLocalCallback** returns ``true`` whenever the
  ``(uniqueID, messageId)`` pair was registered and removed, as documented.
  It used to return ``true`` only when the message id's last function went.
* **UDPProxyServer enforces coordinator authorization.** It accepted
  forwarding requests from any connected system; now only from a coordinator
  it logged in to. Other forwarding messages pass through to the
  application.
* **PacketizedTCP limits message length.** A length prefix above
  ``GetMaxMessageLength()`` (default ``DEFAULT_MAX_MESSAGE_LENGTH``, 64 MiB)
  closes the connection, which then shows up in ``HasLostConnection()``. An
  application that sends larger messages must raise the limit with
  ``SetMaxMessageLength()`` on every side that receives them.
* **Equal-weight messages leave the reliability heaps in FIFO order.** This
  is only visible for sequenced messages that repeat an ordering/sequencing
  index pair while buffered, and for messages of different priorities that
  share a send weight.
* **RakPeer drains at most the commands queued when a cycle starts.** A
  command queued while the network thread is draining waits for the next
  cycle (10 ms at most, sooner for immediate-priority sends). This removed a
  livelock where a thread calling ``CloseConnection()`` in a loop could stall
  the update thread forever.
* **Packets are no longer pooled.** ``Packet`` objects are allocated with
  ``new`` per message. Packets your application never passes to
  ``DeallocatePacket()`` now show as leaks instead of being reclaimed at
  shutdown.
* **Network input is rejected instead of asserted.** ``TableSerializer``,
  ``DataCompressor``, ``StringTable``, ``NatPunchthroughServer``, ``RPC``
  (an ``ID_RPC`` packet without a known sub-identifier) and
  ``RakPeer`` (a truncated ``ID_CONNECTION_REQUEST_ACCEPTED``) used to
  ``RakAssert`` (aborting a Debug build) on malformed data from a peer. They now drop
  the message. ``ConnectionGraph2`` stops at a truncated connection list
  instead of adding unread entries.
* **TableSerializer bounds what it builds.** ``DeserializeTable`` rejects a
  row count larger than the remaining input can hold (8 bytes per row) and a
  table of more than 1,000,000 cells (rows x columns); it used to build up to
  10000 x 100000 cells from under 1 MB of input.
* **Table string cells are not format strings.** ``Table::Cell::ToString``
  returns a ``STRING`` cell's text verbatim; it used to pass it to
  ``RakString``'s printf-style constructor, so a ``%`` in peer-supplied text
  was undefined behaviour.
* **NatPunchthroughServer refuses a punch to oneself.** A request naming the
  sender's own GUID is answered with ``ID_NAT_TARGET_NOT_CONNECTED``; it used
  to be recorded twice and leave a dangling attempt when the sender left.
* **Retail builds keep source locations in some messages.** ``_RETAIL``
  builds used to replace ``__FILE__`` / ``__LINE__`` with ``"", 0`` through
  ``_FILE_AND_LINE_``. Router2 debug messages now
  always carry the real file and line.

See Also
--------

* :doc:`custom-memory-management` - Replacing the allocator
* :doc:`/getting-started/system-overview` - Containers and helpers
* :doc:`/changelog` - Release notes
