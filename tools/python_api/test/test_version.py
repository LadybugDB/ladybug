def test_version() -> None:
    import real_ladybug as lb

    assert lb.version != ""
    assert lb.storage_version > 0
    assert lb.version == lb.__version__
