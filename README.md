# Freeswitch PushNotification module
mod_apn: Push notifications module of VoIP telephony system [Freeswitch](http://freeswitch.org)<br>
Module APN listens to `sofia::register` event, parses `Contact` header from SIP REGISTER and store all necessary information to db.<br>  
In case if Freeswitch generate a call to target, which has stored token(s), endpoint `apn_wait` send http request to push server (cloud based or server based) with all info regarding device (platform, push token and so on)<br>
Mod APN listens to `sofia::register` event and originate call when receive new REGISTER SIP message from target device.
## Dependencies
```
libcurl
```
## Installation
```sh
$ mkdir -p /usr/src && cd /usr/src/
$ git clone https://github.com/sem32/freeswitch-PushNotification.git PushNotification
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
Change apn.conf.xml with your configuration of url to push server and all parameters.
```sh
$ cp /usr/src/PushNotification/conf/autoload_configs/apn.conf.xml /etc/freeswitch/autoload_configs/
```

### Module configuration
```xml
<settings>
    <!-- Connection string to db. mod_apn will create table push_tokens with schema:
    "CREATE TABLE push_tokens ("
        "id             serial NOT NULL,"
        "token          VARCHAR(255) NOT NULL,"
        "extension      VARCHAR(255) NOT NULL,"
        "realm          VARCHAR(255) NOT NULL,"
        "app_id         VARCHAR(255) NOT NULL,"
        "type           VARCHAR(255) NOT NULL,"
        "platform       VARCHAR(255) NOT NULL,"
        "last_update    timestamp with time zone NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "CONSTRAINT push_tokens_pkey PRIMARY KEY (id)
    )"
    -->
    <param name="odbc_dsn" value="pgsql://hostaddr=$${odbc_host} dbname=$${odbc_db} user=$${odbc_user} password=$${odbc_pass} options='-c client_min_messages=NOTICE'" />
    <!-- Name of REGISTER contact parameter, which should contain VOIP token
            value from contact parameter `contact_voip_token_param` will be stored to db with ${type}: `voip` and can be used as
            ${token} in url and/or post parameters
    -->
    <param name="contact_voip_token_param" value="pn-voip-tok"/>
    <!-- Name of REGISTER contact parameter, which should contain IM token
            value from contact parameter `contact_im_token_param` will be stored to db with ${type}: `im` and can be used as
            ${token} in url and/or post parameters
    -->
    <param name="contact_im_token_param" value="pn-im-tok"/>
    <!-- Name of REGISTER contact parameter, which should contain application id
            value from contact parameter `contact_app_id_param` will be stored to db and can be used as
            ${app_id} in url and/or post parameters
    -->
    <param name="contact_app_id_param" value="app-id"/>
    <!-- Name of REGISTER contact parameter, which should contain platform
            value from contact parameter `contact_app_id_param` will be stored to db and can be used as
            ${platform} in url and/or post parameters
    -->
    <param name="contact_platform_param" value="pn-platform"/>
</settings>
```

### Profiles configuration
```xml
<profile name="voip">
    <param name="id" value="0"/>
    <!-- URI template parameter with variables: ${type}, ${user}, ${realm}, ${token}, ${app_id}, ${platform} -->
    <param name="url" value="http://somedomain.com/${type}/${realm}/${user}/${token}/${app_id}/${platform}"/>
    <!-- Supported methods: GET and POST -->
    <param name="method" value="post"/>
    <!-- Optional parameter. Supported auth types: None, JWT, DIGEST, BASIC -->
    <param name="auth_type" value="digest"/>
    <!-- Optional parameter. For JWT add token only, for digest or basic: login:password -->
    <param name="auth_data" value="admin:password"/>
    <!-- Optional parameter. Will be added header Content-Type with value from this parameter -->
    <param name="content_type" value=""/>
    <!-- Optional parameter. Libcurl connect_timeout parameter, sec -->
    <param name="connect_timeout" value="300"/>
    <!-- Optional parameter. CURL timeout parameter, sec -->
    <param name="timeout" value="0"/>
    <!-- Post body template use variables:
            ${type}, - voip or im
            ${app_id}, - application id from db (whatever you set to `contact_app_id_param`)
            ${user}, - user extension number
            ${realm}, - Realm
            ${token}, - token
            ${platform} - platform (whatever you set to `contact_platform_param`)
            ${payload} - json body of payload (cli command apn only)
        Default value: {"type": "${type}",
                        "app":"${app_id}",
                        "token":"${token}",
                        "user":"${user}",
                        "realm":"${realm}",
                        "payload":${payload},
                        "platform":"${platform}"}
    -->
    <param name="post_data_template" value="type=${type}&app_id=${app_id}&user=${user}&realm=${realm}&token=${token}&platform=${platform}&payload=${payload}"/>
</profile>
```

Mod APN support two types of push notification: `voip` and `im`.<br>

#### Templates
You can configure `url` and `body` for your http request with template variables:
 - `${user}` - extension of device tokens owner
 - `${type}` - type of token (voip or im)
 - `${realm}` - realm name
##### Stored in db (from `Contact` parameters of SIP REGISTER)
 - `${token}` - pn-token
 - `${app_id}` - application id
 - `${platform}` -  platform 

Change your dial-string user's parameter for use endpoint `app_wait`
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
</include>
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
Any platform devices (iOS based, Android based, browser based) application sent SIP REGISTER request with custom contact parameters:
```
Contact: "101" <sip:101@192.168.31.100:56568;app-id=****;pn-voip-tok=XXXXXXXXX;pn-im-tok=XXXXXXXXXX;pn-platform=iOS>
```
Mod APN store to db tokens when parse `Contact` header from REGISTER<br>
In case if Freeswitch will genarate to `User 101` a call, endpoint `apn_wait` will send http request to push notification service with token ID and wait for incoming REGISTER request from current user.<br>
After receiving SIP REGISTER, module will originate INVITE to `User 101`.

## Send notification
### From event
#### headers
`type`: 'voip' or 'im'<br>
`realm`: string value of realm name<br>
`user`: string value of user extension<br>
#### body (optional)
JSON object with payload data
`body` - string valueg<br>
`barge` - integer value<br>
`sound` - string value<br>
`content_available` - boolean value<br>
`action_key` - string value<br>
`image` - string value<br>
`category` - string value<br>
`title` - string value<br>
`localized_key` - string value<br>
`localized_args` - json array with string elements<br>
`title_localized_key` - string value<br>
`title_localized_args` - json array with string elements<br>
`custom` - array of json objects, with custom values<br>

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
   Contact: <sip:101@192.168.31.100:64503;transport=TCP;app-id=com.carusto.mobile.app;pn-voip-tok=39f161b205281f890715e625a7093d90af2fa281a7fcda82a7267f93d4b73df1;pn-platform=iOS;ob>;reg-id=1;+sip.instance="<urn:uuid:00000000-0000-0000-0000-0000d2b7e3b3>"
   Expires: 600
   Allow: PRACK, INVITE, ACK, BYE, CANCEL, UPDATE, INFO, SUBSCRIBE, NOTIFY, REFER, MESSAGE, OPTIONS
   Authorization: Digest username="101", realm="local.carusto.com", nonce="16472563-0102-11e7-b187-b112d280470a", uri="sip:local.carusto.com", response="6c53edfe29129b45a57664a3875de0c9", algorithm=MD5, cnonce="PqC351P2x33H2v4m95FoOAXQDxP9ap91", qop=auth, nc=00000001
   Content-Length:  0

```

##### Event for mod_apn
```
Event-Name: CUSTOM
Event-Subclass: mobile::push::notification
type: voip
realm: local.carusto.com
user: 100
app_id: com.carusto.mobile.app

{
  "barge":1,
  "body":"Body message",
  "sound":"default",
  "content_available":true,
  "image":"image.png",
  "category":"VOIP",
  "title":"Some title",
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
$ fs_cli -x 'apn {"type":"voip","realm":"local.carusto.com","user":"100"}'
```
or
```sh
$ fs_cli -x 'apn {"type":"im","payload":{"body":"Text alert message","sound":"default"},"user":"100","realm":"local.carusto.com"}'
```

## Important
Mod APN will send http request for each token of stored user tokens. 