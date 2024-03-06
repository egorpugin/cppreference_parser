#!/bin/bash

DIR=cppreference
for i in `sqlite3 cppreference2.db "select name from page;"`; do
    i="${i%%[[:cntrl:]]}"
    mkdir -p `dirname $DIR/$i`
    sqlite3 cppreference2.db "select source from page where name = '$i';" > $DIR/$i.html
done
