#!/bin/sh
echo "Content-type: text/html"
echo ""
echo ""
echo ""
echo "<html>"
echo "<body>"

echo "<FORM METHOD=GET ACTION=\"http://o2blom.no-ip.org/cgi-bin/button.cgi\">"

#All events
echo "<BR>All Activity"
echo "<table style=\"border-collapse: collapse; border-spacing: 2px; cellSpacing=\"2\" cellPadding=\"4\" border=\"2\">"
echo "<tr><th>Time Passed</th><th>Time</th><th>Type</th><th>Zone</th></tr>"
sqlite3 -html /dscd_db "SELECT time(strftime('%s','now', 'localtime') - strftime('%s',events.time), 'unixepoch'), events.time, eventTypes.name, events.zoneName FROM events INNER JOIN eventTypes ON events.type = eventTypes.type order by events.rowid desc limit 40;"
echo "</table>"

#Arming
echo "<BR>Arming"
echo "<table style=\"border-collapse: collapse; border-spacing: 2px; cellSpacing=\"2\" cellPadding=\"4\" border=\"2\">"
echo "<tr><th>Time Passed</th><th>Time</th><th>Type</th></tr>"
sqlite3 -html /dscd_db "SELECT time(strftime('%s','now', 'localtime') - strftime('%s',events.time), 'unixepoch'), events.time, eventTypes.name FROM events INNER JOIN eventTypes ON events.type = eventTypes.type where events.type in (6,8,11) order by events.rowid desc limit 40;"
echo "</table>"

#Front Door activity
echo "<BR>Front Door"
echo "<table style=\"border-collapse: collapse; border-spacing: 2px; cellSpacing=\"2\" cellPadding=\"4\" border=\"2\">"
echo "<tr><th>Time Passed</th><th>Time</th><th>Type</th></tr>"
sqlite3 -html /dscd_db "SELECT time(strftime('%s','now', 'localtime') - strftime('%s',events.time), 'unixepoch'), events.time, eventTypes.name FROM events INNER JOIN eventTypes ON events.type = eventTypes.type where events.zone = 2 order by events.rowid desc limit 40;"
echo "</table>"

#Back Door activity
echo "<BR>Back Door"
echo "<table style=\"border-collapse: collapse; border-spacing: 2px; cellSpacing=\"2\" cellPadding=\"4\" border=\"2\">"
echo "<tr><th>Time Passed</th><th>Time</th><th>Type</th></tr>"
sqlite3 -html /dscd_db "SELECT time(strftime('%s','now', 'localtime') - strftime('%s',events.time), 'unixepoch'), events.time, eventTypes.name FROM events INNER JOIN eventTypes ON events.type = eventTypes.type where events.zone = 1 order by events.rowid desc limit 40;"
echo "</table>"

#Motion Sensor Activity
echo "<BR>Motion Sensor"
echo "<table style=\"border-collapse: collapse; border-spacing: 2px; cellSpacing=\"2\" cellPadding=\"4\" border=\"2\">"
echo "<tr><th>Time Passed</th><th>Time</th><th>Type</th></tr>"
sqlite3 -html /dscd_db "SELECT time(strftime('%s','now', 'localtime') - strftime('%s',events.time), 'unixepoch'), events.time, eventTypes.name FROM events INNER JOIN eventTypes ON events.type = eventTypes.type where events.zone = 3 order by events.rowid desc limit 40;"
echo "</table>"

echo "<INPUT TYPE=\"submit\" NAME=\"ARMAWAY\" VALUE=\"Arm Away\">"
echo "<INPUT TYPE=\"submit\" NAME=\"ARMHOME\" VALUE=\"Arm Home\">"
echo "<INPUT TYPE=\"submit\" NAME=\"DISARM\" VALUE=\"Disarm\">"
echo "<INPUT TYPE=\"submit\" NAME=\"PANIC\" VALUE=\"Panic\">"

echo "</FORM>"
echo "</html>"
echo "</body>"


echo ""
echo ""
