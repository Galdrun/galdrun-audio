"""Live read/invalid-input smoke check. Does not change appliance settings.

Usage: python3 hardware/buildroot/scripts/test-appliance.py http://DEVICE:4720
"""
import json
import sys
from urllib.request import Request, urlopen

base = sys.argv[1].rstrip('/')


def api(command, params=None):
    request = Request(base + '/api', json.dumps(dict(command=command, params=params or {})).encode(),
                      {'Content-Type': 'application/json'})
    with urlopen(request, timeout=15) as response:
        return json.load(response)


with urlopen(base + '/', timeout=15) as response:
    page = response.read().decode()
    assert 'wifiRadio' in page and 'audioProfile' in page and 'sshEnabled' in page
status = api('status')
assert status['ok'], status
power = api('getPower')
assert power['ok'] and not power['deepSleep'], power
network = api('getNetwork')
assert network['ok'] and network['supported'], network
assert 'password' not in network['network']['wifi'], 'Network password leaked'
bad_radio = api('setNetwork', {'wifi': {'radio': 'wlan0;reboot'}})
assert not bad_radio['ok'], bad_radio
assert network['network'] == api('getNetwork')['network'], 'Invalid radio changed settings'
profiles = api('getAudioProfile')
assert profiles['ok'], profiles
if profiles['supported']:
    assert len(profiles['profiles']) >= 20 and 'usb' in [p['id'] for p in profiles['profiles']]
assert not api('setAudioProfile', {'profile': '../invalid'})['ok']
dac = api('getDac')
assert dac['ok'], dac
if dac['supported']:
    controls = dac['dac']['controls']
    assert controls, dac
    assert all(c['type'] in ('boolean', 'integer', 'enum') for c in controls)
    assert not any(any(word in c['name'].lower() for word in ('clock', 'drift', 'ppm', 'pll', 'sample rate')) for c in controls)
    invalid = {'__invalid_control__': [0]}
    if dac['dac'].get('card'):
        assert not api('setDac', {'card': '__wrong_card__', 'controls': {}})['ok']
        invalid = {'card': dac['dac']['card'], 'controls': invalid}
    assert not api('setDac', invalid)['ok']
    assert controls == api('getDac')['dac']['controls'], 'Invalid DAC write changed controls'
    print('PASS: selected-card DAC enumeration and invalid-batch atomicity:', dac['dac']['codec'], len(controls), 'controls')
cpu = api('getCpu')
assert cpu['ok'], cpu
print('PASS: settings page, radio validation/secret handling, profile gating, CPU and standby capability')
print('Client identity:', status['status']['clientId'])
