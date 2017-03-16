# freeswitch-ApplePushNotification
mod_apn: Apple push notifications module of VoIP telephony system [Freeswitch](http://freeswitch.org), based on [libcapn](http://libcapn.org)<br>
Module APN send a push message to an iOS device, when the device is the target of a bridge call and is not registered, so that the device can "wake up", register and receive the call.<br>
Endpoint `apn_wait` push notification, listens for `sofia::register` event and generate call when receive new register message from target device.
## Dependencies
```
cmake, make, openssl-devel
```
## Installation
```sh
$ mkdir -p /usr/src && cd /usr/src/
$ git clone https://github.com/sem32/freeswitch-ApplePushNotification.git ApplePushNotification
$ ./libcapn/build.sh
# $(PWD_FREESWITCH_SRC) - path to freeswitch source files
$ cp -a ./mod_apn $(PWD_FREESWITCH_SRC)/src/mod/endpoints/
$ cd $(PWD_FREESWITCH_SRC)
# Add to modules.conf parameter for build mod_apn
echo 'endpoints/mod_apn' >> modules.conf
# Add to configure.ac configuration for create Makefile for mod_apn (AC_CONFIG_FILES array section)
$ sed -i '/src\/mod\/endpoints\/mod_sofia\/Makefile/a src\/mod\/endpoints\/mod_apn\/Makefile' configure.ac
$ ./configure
$ make
$ make install
```
## Configuration
### Certificates
```sh
# Create pem file from certificate file
$ openssl x509 -in voip_services.cer -inform der -out PushVoipCert.pem
# Create pem file from exported private key file
$ openssl pkcs12 -nocerts -out PushVoipKey.pem -in Certificates.p12
```
### Configuration file
Change apn.conf.xml with your password, profile name to your apple application id.
```sh
$ cp /usr/src/ApplePushNotification/conf/autoload_configs/apn.conf.xml /etc/freeswitch/autoload_configs/
$ cp ./PushVoipCert.pem ./PushVoipKey.pem /ect/freeswitch/certs/
```
Change your dial-string user's parameter for use endpoint app_wait
```xml
<include>
  <user id="101">
    <params>
	  <!--...-->
	  <param name="dial-string" value="${sofia_contact(${dialed_user}@${dialed_domain})}:_:apn_wait/${dialed_user}@${dialed_domain}"/>
	  <!--...-->
    </params>
    <variables>
	<!--...-->
    </variables>
  </user>
</include>}"/>
```
## Auto load
```sh
$ sed -i '/<load module="mod_sofia"\/>/a <load module="mod_apn"\/>' /ect/freeswitch/modules.conf.xml
```
## Manual load
```sh
$ fs_cli -rx 'load mod_apn'
```
## How it works
iOS application sent SIP registration with custom contact parameters:
```
Contact: "101" <sip:101@192.168.31.100:56568;app-id=****;pn-voip-tok=XXXXXXXXX;pn-im-tok=XXXXXXXXXX>
```
Module use parameters for create database record with Apple Push Notification tokens.
In case User 101 have incoming call endpoint apn_wait send notification to Apple with token ID and wait for incoming register message from current user. In case got REGISTER, module make originate call to User 101.

## Send notification
### From event
#### headers
`type`: 'voip' or 'im'<br>
`realm`: string value of realm name<br>
`user`: string value of user extension<br>
`app_id`: string value with Apple application id
#### body
JSON object with payload data
`body` - type string<br>
`barge` - type integer<br>
`sound` - type string<br>
`content_available` - type boolean<br>
`action_key` - type string<br>
`image` - type string<br>
`category` - type string<br>
`custom` - array of objects, with custom values.<br>
Available types of value for custom data: string, integer, double, boolean, null

#### Examples
##### SIP REGISTER message from iOS device
```
   REGISTER sip:local.carusto.com SIP/2.0
   Via: SIP/2.0/TCP 192.168.31.100:64503;rport;branch=z9hG4bKPjCopvkuNIv-OvRw5doGAOdEiyTYaSyd1W;alias
   Max-Forwards: 70
   From: <sip:101@local.carusto.com>;tag=nyxukpmU0h21yUHcowgbUJs3pqXrOzS6
   To: <sip:101@local.carusto.com>
   Call-ID: CDSaFEyhUvnJARMfMLS.UF6Jkv8PJ6lq
   CSeq: 48438 REGISTER
   Supported: outbound, path
   Contact: <sip:101@192.168.31.100:64503;transport=TCP;app-id=com.carusto.mobile.app;pn-voip-tok=39f161b205281f890715e625a7093d90af2fa281a7fcda82a7267f93d4b73df1;ob>;reg-id=1;+sip.instance="<urn:uuid:00000000-0000-0000-0000-0000d2b7e3b3>"```
   Expires: 600
   Allow: PRACK, INVITE, ACK, BYE, CANCEL, UPDATE, INFO, SUBSCRIBE, NOTIFY, REFER, MESSAGE, OPTIONS
   Authorization: Digest username="101", realm="local.carusto.com", nonce="16472563-0102-11e7-b187-b112d280470a", uri="sip:local.carusto.com", response="6c53edfe29129b45a57664a3875de0c9", algorithm=MD5, cnonce="PqC351P2x33H2v4m95FoOAXQDxP9ap91", qop=auth, nc=00000001
   Content-Length:  0

```

##### Event for mod_apn
```
Event-Name: CUSTOM
Event-Subclass: apple::push::notification
type: voip
realm: carusto.com
user: 100
app_id: com.carusto.mobile.app

{
  "barge":1,
  "body":"Body message",
  "sound":"default",
  "content-available":true,
  "image":"test image",
  "category":"VOIP",
  "custom":[
    {
      "name":"Custom string variable",
      "value":"test string"
    },{
      "name":"Custom integer variable",
      "value":1000
    }
  ]
}
```
### From cli/api command to existing token(s)
```sh
$ fs_cli -x 'apn {"app_id":"com.carusto.mobile.app","type":"voip","payload":{"barge":1,"body":"test","sound":"default","content-available":true,"custom":[{"name":"integer","value":1},{"name":"string","value":"test"},{"name":"double","value":1.2}],"image":"my image","category":"VoIP"},"tokens":["XXXXXX","YYYYYYYY]}'
```
or
```sh
$ fs_cli -x 'apn {"app_id":"com.carusto.mobile.app","type":"im","payload":{"body":"Text alert message","sound":"default"},"tokens":["XXXXXX","YYYYYYYY]}'
```
## Debug
Change debug parameter in /etc/freeswitch/autoload_configs/apn.conf.xml to true
```sh
$ fs_cli -x 'reload mod_apn'
```