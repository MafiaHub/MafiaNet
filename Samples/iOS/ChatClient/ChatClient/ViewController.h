/*
 *  Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 */

//
// Main view controller, that presents the chat
//

#import <UIKit/UIKit.h>

// RakNet headers
#include "mafianet/MessageIdentifiers.h"
#include "mafianet/peerinterface.h"
#include "mafianet/statistics.h"
#include "mafianet/types.h"
#include "mafianet/BitStream.h"
#include "mafianet/PacketLogger.h"
#include <assert.h>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "mafianet/types.h"

// For simplicity, this sample doesn't support secure connections
#if LIBCAT_SECURITY==1
#error RakNet secure connections not supported for this sample. If you wish to add support, check the "Chat Example Client" sample
#endif

#import "ChatServerDetailsProtocol.h"

@interface ViewController : UIViewController<ChatServerDetailsProtocol>
{
    RakNet::RakPeerInterface *mRakPeer;
    UITextField *mSendText;
    UITextView *mTextBox;
}

-(void)appendMessage:(NSString*)message;
-(IBAction)sendMessage;
-(void)tickClient; // Updates the client

@property (nonatomic, retain) IBOutlet UITextField *mSendText;
@property (nonatomic, retain) IBOutlet UITextView *mTextBox;

@end
