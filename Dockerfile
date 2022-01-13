FROM ubuntu:18.04
RUN apt-get --yes update --fix-missing
RUN apt-get --yes install strace libconfig9 busybox sudo inotify-tools ipsvd iproute2 gdb
RUN adduser --home /home/devel --shell /bin/bash --gecos '' --disabled-password devel
RUN /bin/bash -c 'echo -e "devel\ndevel\n"' | passwd devel
RUN adduser devel sudo

#FROM busybox:glibc
#RUN adduser -h /home/devel -s /bin/sh -D devel devel
#RUN echo -e 'devel\ndevel\n' | passwd devel
