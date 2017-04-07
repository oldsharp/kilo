def FlagsForFile(filename, **kwargs):
    return {"flags": ["-x", "c", "-Wall", "-Wextra", "-pedantic", "-std=c99"]}
