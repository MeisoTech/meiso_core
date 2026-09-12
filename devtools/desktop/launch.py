#!/usr/bin/env python3
"""Portable socket setup for the installed Meiso Host/Edge service. No wire codec."""
import argparse
import ipaddress
import os
import socket


def port(text):
    value=int(text)
    if not 1<=value<=65535:raise argparse.ArgumentTypeError('port must be 1..65535')
    return value


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role',choices=('host','edge'),required=True)
    p.add_argument('--service',required=True)
    p.add_argument('--bind',type=ipaddress.IPv4Address,default=ipaddress.IPv4Address('127.0.0.1'))
    p.add_argument('--peer',type=ipaddress.IPv4Address,required=True)
    p.add_argument('--control-port',type=port,required=True)
    p.add_argument('--media-port',type=port,required=True)
    p.add_argument('--peer-media-port',type=port,required=True)
    p.add_argument('service_args',nargs=argparse.REMAINDER)
    a=p.parse_args()
    args=a.service_args[1:] if a.service_args[:1]==['--'] else a.service_args
    if len(args)%2 or any(args[i] in {'--control-fd','--media-fd','--role'} for i in range(0,len(args),2)):
        p.error('service options must be name/value pairs; role and descriptors belong to launcher')
    with socket.socket() as control, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as media:
        media.bind((str(a.bind),a.media_port))
        media.connect((str(a.peer),a.peer_media_port))
        if a.role=='host':
            control.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            control.bind((str(a.bind),a.control_port));control.listen(1);control.settimeout(60)
            print('HOST_LISTENING',flush=True)
            connected,peer=control.accept()
            if peer[0]!=str(a.peer):connected.close();raise RuntimeError('unexpected peer locator')
        else:
            control.settimeout(10);control.connect((str(a.peer),a.control_port));connected=control
        with connected:
            connected.setsockopt(socket.SOL_SOCKET,socket.SO_KEEPALIVE,1)
            for name,value in [('TCP_KEEPIDLE',5),('TCP_KEEPINTVL',2),('TCP_KEEPCNT',3),('TCP_USER_TIMEOUT',15000)]:
                if hasattr(socket,name):connected.setsockopt(socket.IPPROTO_TCP,getattr(socket,name),value)
            for channel in (connected,media):channel.setblocking(False);channel.set_inheritable(True)
            os.execv(a.service,[a.service,'--role',a.role,'--control-fd',str(connected.fileno()),
                               '--media-fd',str(media.fileno()),*args])

if __name__=='__main__':main()
