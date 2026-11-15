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
#include "mafianet/message_identifiers.h"
#include "mafianet/peer_interface.h"
#include "mafianet/statistics.h"
#include "mafianet/types.h"
#include "mafianet/bit_stream.h"
#include "mafianet/packet_logger.h"
#include <assert.h>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include "mafianet/types.h"

// Connections are encrypted by default; nothing to configure here.

#import "ChatServerDetailsProtocol.h"

@interface ViewController : UIViewController<ChatServerDetailsProtocol>
{
    MafiaNet::RakPeerInterface *mRakPeer;
    UITextField *mSendText;
    UITextView *mTextBox;
}

-(void)appendMessage:(NSString*)message;
-(IBAction)sendMessage;
-(void)tickClient; // Updates the client

@property (nonatomic, retain) IBOutlet UITextField *mSendText;
@property (nonatomic, retain) IBOutlet UITextView *mTextBox;

@end
