#!/bin/bash
echo "Content-type: text/html"
echo ""
echo ""
echo ""

echo "<html>"
echo "<body>"

echo "<font size=\"7\">"

if [ $QUERY_STRING = "ARMAWAY=Arm+Away" ]
then
        echo -e "ARMAWAY1\r" | nc 127.0.0.1 832 -w 1
elif [ $QUERY_STRING = "ARMHOME=Arm+Home" ]
then
        echo -e "ARMSTAY1\r" | nc 127.0.0.1 832 -w 1
elif [ $QUERY_STRING = "DISARM=Disarm" ]
then
        echo -e "DISARM1\r" | nc 127.0.0.1 832 -w 1
elif [ $QUERY_STRING = "PANIC=Panic" ]
then
        echo -e "PANIC1\r" | nc 127.0.0.1 832 -w 1
fi

echo "</font>"
echo "</html>"
echo "</body>"


