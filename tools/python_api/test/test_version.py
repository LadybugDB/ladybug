def test_version() -> None:
    import lbug

    assert lbug.version != ""
    assert lbug.storage_version > 0
    assert lbug.version == lbug.__version__
