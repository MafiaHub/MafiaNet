EmailSender
===========

The EmailSender utility provides SMTP client functionality for sending emails
directly from your game server. Useful for account verification, password
recovery, and administrative notifications.

Basic Usage
-----------

**Sending a simple email:**

.. code-block:: cpp

   #include "slikenet/EmailSender.h"

   MafiaNet::EmailSender emailSender;

   const char* result = emailSender.Send(
       "smtp.example.com",           // SMTP server
       25,                           // Port (25, 465, or 587)
       "gameserver@example.com",     // Sender
       "Game Server",                // Sender name
       "player@example.com",         // Recipient
       "Player Name",                // Recipient name
       "Welcome to Our Game!",       // Subject
       "Thank you for registering!", // Body
       nullptr,                      // Attachments (FileList*)
       false,                        // HTML body?
       "username",                   // SMTP username (optional)
       "password"                    // SMTP password (optional)
   );

   if (result != nullptr) {
       printf("Email send failed: %s\n", result);
   } else {
       printf("Email sent successfully\n");
   }

**Sending HTML email:**

.. code-block:: cpp

   const char* htmlBody =
       "<html><body>"
       "<h1>Welcome!</h1>"
       "<p>Click <a href='https://game.example.com/verify?token=abc123'>"
       "here</a> to verify your account.</p>"
       "</body></html>";

   emailSender.Send(
       "smtp.example.com", 587,
       "noreply@example.com", "Game Server",
       playerEmail, playerName,
       "Verify Your Account",
       htmlBody,
       nullptr,
       true,  // HTML body
       smtpUser, smtpPass
   );

**Sending with attachments:**

.. code-block:: cpp

   #include "slikenet/FileList.h"

   MafiaNet::FileList attachments;
   attachments.AddFile("report.txt", "report.txt",
                       FileListNodeContext(0, 0, 0, 0));

   emailSender.Send(
       "smtp.example.com", 587,
       "server@example.com", "Game Server",
       "admin@example.com", "Admin",
       "Daily Player Report",
       "Please see attached report.",
       &attachments,
       false,
       smtpUser, smtpPass
   );

Key Features
------------

* SMTP protocol support
* Authentication (PLAIN, LOGIN)
* HTML and plain text bodies
* File attachments
* Multiple recipients via CC/BCC
* TLS support (port 587)

Common SMTP Ports
-----------------

* **25** - Standard SMTP (often blocked by ISPs)
* **465** - SMTPS (implicit SSL)
* **587** - Submission (STARTTLS)

Configuration Options
---------------------

* ``SetSMTPServer()`` - Set server address
* ``SetSenderAddress()`` - Set from address
* ``AddRecipient()`` - Add to/cc/bcc recipients
* ``SetSubject()`` - Set email subject
* ``SetBody()`` - Set email body
* ``AttachFile()`` - Add attachment

Error Handling
--------------

The ``Send()`` function returns:

* ``nullptr`` - Success
* Error string - Description of failure

Common errors:

* "Connection failed" - Cannot reach SMTP server
* "Authentication failed" - Invalid credentials
* "Recipient rejected" - Invalid email address

See Also
--------

* :doc:`tcp-interface` - Underlying TCP transport
* :doc:`../plugins/file-list-transfer` - File handling
* :doc:`../plugins/lobby2` - User account management
