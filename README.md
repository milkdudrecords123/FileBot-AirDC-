add these files for win 11 in the install dir

libssl-3-x64.dll

libcrypto-3-x64.dll
login configure file
config.json
put in the login infomaiton USER NAME,PASSWORD and the hub address ( only works with this adcs://)

"bot_nick": "USER NAME",
-----------------------------
"bot_password": "PASSWORD",
-----------------------------
"cert_file": "client.crt.txt",
"connection": {
    "external_ip": "",
    "mode": "active"
},
"feeds_file": "feeds.json",
---------------------------
"hub_url": "adcs://HUB-ADDRESS:1234/?kp=SHA256/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
---------------------------
"key_file": "client.key.txt",
"ports": {
    "tcp": ,
    "tls_tcp": ,
    "udp": 
},
"refresh_minutes": 10,
"users_file": "rss_users.json"

DC++ client.key and client.crt required

you need to get them from DC++

https://dcplusplus.sourceforge.io/img/screenshots/settings/settings_adcs.png
C:\Users\YOURPC\AppData\Roaming\DC++\Certificates
COPY FILES MOVE TO INSTALL DIR AND RENAME

you need to rename them to this
client.crt.txt client.key.txt
make a folder name it nlohmann

add this file in it json.hpp

Compilation command

g++ -static-libgcc -static-libstdc++ -Wno-register -o bot.exe main.cpp release.cpp adclib_core.cpp base32.cpp tiger.cpp -lssl -lcrypto -lws2_32 -lcurl
