// Links against an installed MafiaNet and exercises it once: create a peer,
// start it (which initialises libsodium), shut it down. Exit code 0 on success.
#include "mafianet/peer_interface.h"

#include <cstdio>

int main()
{
	MafiaNet::RakPeerInterface *peer = MafiaNet::RakPeerInterface::GetInstance();
	if (peer == nullptr)
		return 1;
	MafiaNet::SocketDescriptor descriptor;
	const MafiaNet::StartupResult result = peer->Startup(1, &descriptor, 1);
	peer->Shutdown(0);
	MafiaNet::RakPeerInterface::DestroyInstance(peer);
	if (result != MafiaNet::RAKNET_STARTED)
	{
		std::printf("Startup failed: %d\n", (int) result);
		return 1;
	}
	std::printf("MafiaNet consumer OK\n");
	return 0;
}
