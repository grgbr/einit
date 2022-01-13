#!/bin/sh -e

# Image was built using
#docker build --file Dockerfile --tag tinit-bbox:latest .
#docker build --file Dockerfile --tag tinit-ubuntu:18.04 .

#destdir=$(make -C $(dirname $0) -qinrRp | \
#          sed -n '/^DESTDIR[[:blank:]:=]*/s/^[^=]\+=[[:blank:]]*//p')

#destdir=$(pwd)/out/root
#
#exec docker run \
#            --privileged \
#            --interactive=true \
#            --tty=true \
#            -v $destdir:/mnt \
#            --rm \
#            tinit-ubuntu:18.04 \
#            $*

docker kill tinit 2>/dev/null || true
docker rm tinit 2>/dev/null || true
docker container create --name="tinit" --privileged --interactive --tty --rm \
	tinit-ubuntu:18.04 /sbin/init
docker container cp -aL $(pwd)/out/root/sbin tinit:/
docker container cp -aL $(pwd)/out/root/lib tinit:/
docker container cp -aL $(pwd)/tinit/root/etc tinit:/
docker container cp -aL $(pwd)/tinit/root/sbin tinit:/
exec docker container start --attach --interactive tinit
