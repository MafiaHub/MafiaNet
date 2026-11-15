/*
 *  Original work: Copyright (c) 2014, Oculus VR, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  RakNet License.txt file in the licenses directory of this source tree. An additional grant 
 *  of patent rights can be found in the RakNet Patents.txt file in the same directory.
 *
 *
 *  Modified work: Copyright (c) 2016-2017, SLikeSoft UG (haftungsbeschränkt)
 *
 *  This source code was modified by SLikeSoft. Modifications are licensed under the MIT-style
 *  license found in the license.txt file in the root directory of this source tree.
 */

#include "mafianet/email_sender.h"
#include "mafianet/file_list.h"
#include <stdio.h>
#include "mafianet/gets.h"
#include "mafianet/linux_adapter.h"
#include "mafianet/osx_adapter.h"

int main()
{
	printf("A C++ class used to send email, such as for servers.\n");
	printf("Connections are plaintext (no TLS), so the mail server must accept unencrypted SMTP.\n");
	printf("Difficulty: Beginner\n\n");

	MafiaNet::FileList fileList;
	MafiaNet::EmailSender emailSender;
	const char *quote = "Man is distinguished, not only by his reason, but by this singular passion from other animals, which is a lust of the mind, that by a perseverance of delight in the continued and indefatigable generation of knowledge, exceeds the short vehemence of any carnal pleasure.";
	//	const char base64Map[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	//	char output[1024];
	//	emailSender.Base64Encoding(quote, strlen(quote), output, base64Map);
	//	printf("%s", output);

	char mailServer[128], senderUsername[128], receiver[128];
	printf("Tests sending email.\n");
	printf("Enter mail server: ");
	Gets(mailServer, sizeof(mailServer));
	if (mailServer[0] == 0)
		strcpy_s(mailServer, "localhost");
	printf("Enter email account username: ");
	Gets(senderUsername, sizeof(senderUsername));
	if (senderUsername[0] == 0)
		strcpy_s(senderUsername, "subspacegod@gmail.com");
	printf("Enter receiver email address: ");
	Gets(receiver, sizeof(receiver));
	if (receiver[0] == 0)
		strcpy_s(receiver, "rakkar@rakkar.org");


	const unsigned short hostPort = 25;

	fileList.AddFile("quote.txt", "quote.txt", quote, (const unsigned int) strlen(quote), (const unsigned int) strlen(quote), FileListNodeContext(0, 0, 0, 0), false);
	const char *sendResult = emailSender.Send(mailServer,
	                                          hostPort,
	                                          senderUsername,
	                                          receiver,
	                                          senderUsername,
	                                          receiver,
	                                          "Test subject.",
	                                          "Test attachment body :).\n.\n..\n.\n(Should be .,.,..,.)\r\n.\r\n.\r\n..\r\n.\r\n(Should be .,.,..,.)12345\r\n.\r\n",
	                                          &fileList,
	                                          true,
	                                          0);
	if (sendResult != 0)
		printf("Send Failed! %s", sendResult);
	else
		printf("Success (probably).\n");
	printf("Press enter to quit.\n");
	char buff[256];
	Gets(buff, sizeof(buff));

	return 0;
}
