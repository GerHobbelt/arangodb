#!/usr/bin/python3

from yaml import load, dump, YAMLError
try:
    from yaml import CLoader as Loader, CDumper as Dumper
except ImportError:
    from yaml import Loader, Dumper

import os, re, sys

# List files in Documentation/Metrics:
files = os.listdir("Documentation/Metrics")
files.sort()

# Read list of metrics from source:
s = open("arangod/RestServer/Metrics.cpp")

while True:
    l = s.readline()
    if l == "":
        print("Did not find metricsNameList in arangod/RestServer/Metrics.cpp!")
        sys.exit(2)
    if l.find("metricsNameList") >= 0:
        break

metricsList = []
while True:
    l = s.readline()
    if l.find("nullptr") >= 0:
        break
    pos1 = l.find('"')
    pos2 = l.find('"', pos1+1)
    if pos1 < 0 or pos2 < 0:
        print("Did not find quoted name in this line:\n" + l)
        sys.exit(3)
    metricsList.append(l[pos1+1:pos2])

s.close()

# Check that every listed metric has a .yaml documentation file:
missing = False
yamls = []
for i in range(0, len(metricsList)):
    if not metricsList[i] + ".yaml" in files:
        print("Missing metric documentation for metric '" + metricsList[i] + "'")
        missing = True
    else:
        # Check yaml:
        filename = "Documentation/Metrics/" + metricsList[i] + ".yaml"
        try:
            s = open(filename)
        except FileNotFoundError:
            print("Could not open file '" + filename + "'")
            missing = True
            continue
        try:
            y = load(s, Loader=Loader)
        except YAMLError as err:
            print("Could not parse YAML file '" + filename + "', error:\n" + str(err))
            missing = True
            continue

        yamls.append(y)   # for later dump

        # Check a few things in the yaml:
        for attr in ["name", "help", "exposedBy", "description"]:
            if not attr in y:
                print("YAML file '" + filename + "' does not have required attribute '" + attr + "'")
                missing = True
        
# Dump what we have:
output = dump(yamls, Dumper=Dumper)
s = open("Documentation/Metrics/allMetrics.yaml", "w")
s.write(output)
s.close()

if missing:
    sys.exit(17)
