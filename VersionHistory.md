## 0.3.0 ##

Switched to Hybi WebSocket protocol

Updated to new APIs: "kink-list", "info-list", "character-info"

Icon tags enabled in private IMs and channel descriptions

Fixed bbcode not being parsed in status messages when retrieving a profile

Added an account option: "Use Secure Connections"

Added new RTB messages (thanks to Aniko)

Added the /warn command (thanks to Aniko)

Added the /bottle command

Added the /showdescription command

Added the /who command

NOTE: The version of NSS distributed with the Windows version of Pidgin is outdated. Because of this, Pidgin is unable to make secure connections to the F-List server. The F-List server now requires https to fetch icons, so Pidgin is unable to obtain icons from F-List. The Pidgin developers have stated they will include an updated version of NSS with the next version, but there is no indication when this will be.

This is not the fault of this plugin and there's little I can do.
I'm looking into solutions.
I might distribute a later version of NSS compiled for Windows, along with the corresponding Pidgin plugins (ssl.dll and ssl-nss.dll).

## 0.2.7 ##

Fixed support for the friends list and bookmarks. There were complaints from the F-List staff that the use of the API in 0.2.6 was too aggressive, so the previous version had been blocked. The API has been restructured to be more efficient and the plugin no longer polls for changes.

Support for friends list related RTB packets is added. If someone requests to be your friend or accepts your request to be friends, Pidgin will update instantly.

Changed the status system. There is no longer any option to sync to Pidgin's status.

Improved searches on F-Chat. The proper channel titles are displayed for private rooms (rather than internal names) and you can now choose up to three kinks.

## 0.2.6 ##

Added support for the friends list and bookmarks. The plugin has account options for downloading your friends list (enabled by default) and downloading your bookmarks (disabled by default), and you can add and remove friends and bookmarks by right clicking on buddies in your buddy list.

Added experimental support for icon tags in chat channels.

Fixed /ad from deleting new lines.

Other minor bug fixes.

## 0.2.5 ##

Added options to select the server to connect to. The development server is currently wiki.f-list.net, port 9722.

Added the /getdescription command, like on the official client.

Fixed displaying when users are kicked or banned from channels.

## 0.2.4 ##

Added callbacks for the server's (new) channel invitation packet.

Added the /priv command, like on the official client.

Added a /whoami command, that displays your current account and status.

## 0.2.3 ##

Updated to the server's new roleplay advertisement method.

The new commands are /ad `<Message>`, /showads, /hideads, /showchat, /hidechat.

## 0.2.2 ##

Updated to the server's new chat invitation method.

Added callbacks to staff call packets.

Added clickable hyperlinks to join channels and handle staff calls.
(This requires linking to libpidgin, so the plugin is no longer compatible with other libpurple-based programs.)

Added the /code command.

Altered the BBCode parser to be more forgiving.

Minor bugfixes.

## 0.2.1 ##

Fixed outgoing messages from being locally interpreted as HTML.

Enabled the Hash login method.

## 0.2.0 ##

Initial release.