# freeswitch-ApplePushNotification
mod_apn: Apple push notifications module of VoIP telephony system [Freeswitch](http://freeswitch.org)<br>
libcapn: A simple C Library for interact with the Apple Push Notification Service (APNs) [libcapn](http://libcapn.org)
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

#### Example
```sh
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
  "content_available":true,
  "action_key":"1",
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
### From cli/api command to existing token(s) (for debug and test)
```sh
$ fs_cli -x 'apn {"app_id":"com.carusto.mobile.app","type":"voip","payload":{"barge":1,"body":"test","sound":"default","content_available":true,"custom":[{"name":"integer","value":1},{"name":"string","value":"test"},{"name":"double","value":1.2}],"image":"my image","category":"VoIP"},"tokens":["XXXXXX","YYYYYYYY]}'
```
or
```sh
$ fs_cli -x 'apn {"app_id":"com.carusto.mobile.app","type":"im","payload":{"body":"Text alert message","sound":"default"},"tokens":["XXXXXX","YYYYYYYY]}'
```
## Debug
Change debug parameter in /etc/freeswitch/autoload_configs/apn.conf.xml to true
```sh
fs_cli -x 'reload mod_apn'
```