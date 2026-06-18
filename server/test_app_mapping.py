from app import _map_reading


def test_map_reading_translates_channels_and_keeps_tensao():
    out = _map_reading({"mac": "AA:BB:CC:DD:EE:01", "ch1": 10, "ch2": 20, "ch3": 30, "tensao": 3.30})
    assert out == {"mac": "aa:bb:cc:dd:ee:01", "CH1": 10, "CH2": 20, "CH3": 30, "tensao": 3.30}


def test_map_reading_returns_none_without_mac():
    assert _map_reading({"ch1": 1}) is None
