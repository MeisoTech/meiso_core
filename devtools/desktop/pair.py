#!/usr/bin/env python3
"""Run installed Host/Edge programs over real loopback TCP/UDP, with finite input."""
import argparse
import json
from pathlib import Path
import socket
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-service',type=Path,required=True)
    parser.add_argument('--edge-service',type=Path,required=True)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output-directory',type=Path,required=True)
    parser.add_argument('--frames',type=int,default=60)
    args=parser.parse_args()
    if not 1<=args.frames<=1800:parser.error('frames must be 1..1800')
    args.output_directory.mkdir(parents=True,exist_ok=False,mode=0o700)
    sockets=[];children=[]
    try:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0));listener.listen(1)
            edge_control=socket.socket();edge_control.connect(listener.getsockname())
            sockets.append(edge_control)
            host_control,_=listener.accept();sockets.append(host_control)
        host_media=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sockets.append(host_media)
        edge_media=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);sockets.append(edge_media)
        host_media.bind(('127.0.0.1',0));edge_media.bind(('127.0.0.1',0))
        host_media.connect(edge_media.getsockname());edge_media.connect(host_media.getsockname())
        for channel in sockets:channel.setblocking(False)
        for role,service,control,media,extra in [
            ('host',args.host_service,host_control,host_media,['--source',str(args.source.resolve())]),
            ('edge',args.edge_service,edge_control,edge_media,['--output-directory',str(args.output_directory.resolve()/'edge')])]:
            with (args.output_directory/f'{role}.log').open('w') as log:
                child=subprocess.Popen([str(service.resolve()),'--role',role,
                    '--control-fd',str(control.fileno()),'--media-fd',str(media.fileno()),
                    '--frames',str(args.frames),*extra],pass_fds=(control.fileno(),media.fileno()),
                    stdout=log,stderr=subprocess.STDOUT)
                children.append(child)
        for channel in sockets:channel.close()
        codes=[child.wait(timeout=20+args.frames/30) for child in children]
        result={'host_exit':codes[0],'edge_exit':codes[1],
                'evidence':'SIMULATED','physical_presentation':False,'network':'loopback TCP/UDP'}
        (args.output_directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result))
        return 0 if codes==[0,0] else 1
    finally:
        for channel in sockets:channel.close()
        for child in children:
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait(timeout=5)

if __name__=='__main__':raise SystemExit(main())
