#include "mafianet/peerinterface.h"

int main() {
    MafiaNet::RakPeerInterface* peer = MafiaNet::RakPeerInterface::GetInstance();
    MafiaNet::RakPeerInterface::DestroyInstance(peer);
    return 0;
}
