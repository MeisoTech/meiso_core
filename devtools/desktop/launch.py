#!/usr/bin/env python3
"""Portable socket setup for the installed Meiso Host/Edge service. No wire codec."""
import argparse
import ipaddress
import os
import socket
import subprocess
import sys
import signal


def port(text):
    value=int(text)
    if not 1<=value<=65535:raise argparse.ArgumentTypeError('port must be 1..65535')
    return value


def stop_capture(signum, frame):
    raise SystemExit(128+signum)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--role',choices=('host','edge'),required=True)
    p.add_argument('--service',required=True)
    p.add_argument('--bind',type=ipaddress.IPv4Address,default=ipaddress.IPv4Address('127.0.0.1'))
    p.add_argument('--peer',type=ipaddress.IPv4Address,required=True)
    p.add_argument('--control-port',type=port,required=True)
    p.add_argument('--camera-port',type=port)
    p.add_argument('--camera-device',help='Explicit FFmpeg AVFoundation video device (macOS) or V4L2 path (Linux); no audio')
    p.add_argument('--camera-test-pattern',action='store_true',help='Synthetic FFmpeg test pattern instead of a physical camera')
    p.add_argument('--media-port',type=port,required=True)
    p.add_argument('--peer-media-port',type=port,required=True)
    p.add_argument('service_args',nargs=argparse.REMAINDER)
    a=p.parse_args()
    if (a.camera_device and a.camera_test_pattern) or bool(a.camera_port)!=bool(a.camera_device or a.camera_test_pattern) or (a.camera_port and a.role!='edge'):
        p.error('camera-port requires exactly one camera-device or camera-test-pattern on Edge')
    args=a.service_args[1:] if a.service_args[:1]==['--'] else a.service_args
    if len(args)%2 or any(args[i] in {'--control-fd','--media-fd','--role','--camera-input-fd','--camera-media-fd'} for i in range(0,len(args),2)):
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
            command=[a.service,'--role',a.role,'--control-fd',str(connected.fileno()),
                               '--media-fd',str(media.fileno()),*args]
            if not a.camera_port:os.execv(a.service,command)
            if a.camera_test_pattern:
                source=['-re','-f','lavfi','-i','testsrc2=size=320x240:rate=5']
            elif sys.platform=='darwin':
                source=['-f','avfoundation','-framerate','30','-video_size','640x480','-i',a.camera_device+':none']
            elif sys.platform.startswith('linux'):
                source=['-f','v4l2','-framerate','30','-video_size','640x480','-i',a.camera_device]
            else:raise RuntimeError('camera capture supports macOS AVFoundation or Linux V4L2')
            print('CAMERA_INPUT synthetic='+str(a.camera_test_pattern).lower()+' physical_camera='+str(not a.camera_test_pattern).lower(),flush=True)
            signal.signal(signal.SIGTERM,stop_capture)
            capture=None;edge=None
            with socket.create_connection((str(a.peer),a.camera_port),timeout=10) as camera:
                camera.setblocking(False)
                try:
                    capture=subprocess.Popen(['ffmpeg','-hide_banner','-nostdin',*source,'-an','-vf',
                        'fps=5,scale=320:240','-pix_fmt','rgba','-f','rawvideo','pipe:1'],stdout=subprocess.PIPE)
                    command += ['--camera-media-fd',str(camera.fileno()),'--camera-input-fd',str(capture.stdout.fileno())]
                    edge=subprocess.Popen(command,pass_fds=(connected.fileno(),media.fileno(),camera.fileno(),capture.stdout.fileno()))
                    capture.stdout.close()
                    raise SystemExit(edge.wait())
                finally:
                    for child in (edge,capture):
                        if child and child.poll() is None:
                            child.terminate()
                            try:child.wait(timeout=5)
                            except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)

if __name__=='__main__':main()
