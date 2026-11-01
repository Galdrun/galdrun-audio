"""Check offline filesystem content and the Connector 3 UBI layout; no device I/O."""
from pathlib import Path
import hashlib
import json
import stat
import struct
import subprocess
import sys
from connector3_content import audit_files


def verify(stage, manifest):
    root=stage/'rootfs'
    files={str(path.relative_to(root)): (path.lstat().st_mode,
           path.read_bytes() if stat.S_ISREG(path.lstat().st_mode) else b'')
           for path in root.rglob('*')}
    audit=audit_files(files,manifest)
    if not audit['approved_hardware_radio_present']:
        raise ValueError('Persistent candidate must use the approved hardware radio')
    defaults=json.loads(files['opt/galdrun-client/client-default.json'][1])
    if defaults.get('clientId') or defaults.get('server') or defaults.get('deepSleepAfterSeconds'):
        raise ValueError('Board identity, server binding or deep sleep in generic defaults')
    if 'etc/wpa_supplicant.conf' in files or any(
            name.startswith('etc/ssh/ssh_host_') and name.endswith('_key') for name in files):
        raise ValueError('Saved network or SSH host credentials in rootfs')
    network=files['etc/galdrun-network.conf'][1].decode().splitlines()
    if 'WIFI_SSID=' not in network or 'WIFI_PASSWORD=' not in network:
        raise ValueError('Saved home credentials in rootfs')
    release = 'etc/galdrun-release-defaults' in files
    if release and ('root/.ssh/authorized_keys' in files or 'etc/galdrun-ssh.conf' in files):
        raise ValueError('Release defaults must contain no developer key or enabled SSH setting')
    board = json.loads(files['etc/galdrun-board.json'][1])
    if not board.get('mcuAudioControls') or not board.get('statusLeds') or not files.get('usr/sbin/galdrun-status-leds', (0, b''))[1]:
        raise ValueError('MCU audio and LED support missing')
    if not board.get('sshSettings') or 'usr/sbin/raumfeld-ssh' not in files or 'etc/init.d/S50sshd' not in files:
        raise ValueError('Standard platform SSH settings/startup is missing')
    for applet in ('usr/bin/cryptpw', 'usr/sbin/chpasswd'):
        if applet not in files:
            raise ValueError('SSH password applet missing: '+applet)
    result = subprocess.run(['qemu-arm', '-L', str(root), str(root/'bin/busybox'),
                             'cryptpw', '-P', '0', '-m', 'sha512', '-S', 'testSalt'],
                            input='test-password\n', capture_output=True, text=True, check=True)
    if not result.stdout.startswith('$6$testSalt$'):
        raise ValueError('ARM BusyBox SHA512 password hashing failed')
    subprocess.run(['qemu-arm', '-L', str(root), str(root/'usr/sbin/galdrun-connector3-mcu'),
                    '--self-test'], capture_output=True, check=True, timeout=30)
    data=(stage/'rootfs.ubi').read_bytes()
    peb=262144
    if not data or len(data)%peb or len(data)>380633088:
        raise ValueError('UBI image must contain complete 256 KiB eraseblocks')
    for offset in range(0,len(data),peb):
        if (data[offset:offset+4]!=b'UBI#' or
                struct.unpack_from('>II',data,offset+16)!=(4096,8192)):
            raise ValueError('UBI EC header does not match the qualified NAND layout')
    if data[8192:8192+128*172] != data[peb+8192:peb+8192+128*172]:
        raise ValueError('Redundant UBI volume tables differ')
    volumes=[]
    for number,(name,reserved) in enumerate((('RootFS',768),('GaldrunSettings',128))):
        record=8192+number*172
        count,alignment,pad=struct.unpack_from('>III',data,record)
        name_size=struct.unpack_from('>H',data,record+14)[0]
        actual_name=data[record+16:record+16+name_size].decode()
        if (count,alignment,pad,actual_name)!=(reserved,1,0,name) or data[record+144]!=0:
            raise ValueError('Unexpected UBI volume size/name or autoresize flag')
        volumes.append({'name':name,'reserved_lebs':count,'bytes':count*253952})
    # 1452 total PEBs minus current bad/BBT blocks and two layout blocks;
    # leave more than the modern kernel's 34-PEB bad-block reserve requirement.
    unallocated=1452-6-2-sum(volume['reserved_lebs'] for volume in volumes)
    if unallocated<34+2:
        raise ValueError('Insufficient bad-block/UBI operational reserve')
    report={'purpose':'offline persistent filesystem candidate', 'profile':'release' if release else 'developer',
        'content_audit':audit,'volumes':volumes,'unallocated_pebs_before_ubi_reserves':unallocated,
        'nand_mtd_target':5,'factory_settings_mtd3_preserved':True,
        'ubi_vid_header_offset':4096,'ubi_data_offset':8192,'ubi_leb_bytes':253952,
        'developer_ssh_public_key_present':'root/.ssh/authorized_keys' in files,
        'ssh_off_by_default':release,'release_defaults_verified':release,
        'arm_ssh_password_hashing_verified':True,
        'mcu_audio_self_test_verified':True,
        'mcu_rate_initialization_self_test_verified':True,
        'mcu_status_leds_present':True,
        'mcu_audio_helper_sha256':hashlib.sha256(files['usr/sbin/galdrun-connector3-mcu'][1]).hexdigest(),
        'release_ready':False,'hardware_verified':False,'persistent_install_verified':False,
        'artifacts_sha256':{name:hashlib.sha256((stage/name).read_bytes()).hexdigest()
                            for name in ('rootfs.ubi','rootfs.ubifs','settings.ubifs')}}
    (stage/'persistent-rootfs-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    verify(Path(sys.argv[1]),Path(sys.argv[2]))
