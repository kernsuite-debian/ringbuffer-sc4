# ringbuffer-sc4

This code is part of the realtime pipeline for the AA-ALERT project.
Its purpose is to copy data from the network (UDP packets) into a ringbuffer (provided by the PSRdada package), and do some checking.
See the technical documentation in the `doc` directory.

Packages are read and processed in batches; additional performance tuning on the OS level was needed to handle the approx. 2.5GB per second requiered.

# Installation

Requirements:
 * Cmake
 * Cuda 
 * Psrdada
 
 Instructions:
 
```
$ mkdir build && cd build
$ cmake .. -DCMAKE_BUILD_TYPE=release
$ make
$ make install
```

## Linking to PSRDADA
You can link to a local installation of PSRDADA by setting the `LD_LIBRARY_PATH` and `PSRDADA_INCLUDE_DIR` enviroment variables.


# Usage
Commandline arguments:

  * `-h <heaer_file>` A file containing metadata, it will be read and entered as header into the ringbuffer.
  * `-k <hexadecimal_key>` The key identifying the ringbuffer. It is parsed using sscanf so hexadecimal (0xdada) notation is allowed.
  * `-s <start packet number (long)>` The packet number (ie. timestamp, see documentation) where the observation starts.
  * `-d duration in seconds (float)>` The duration of the observation in seconds.
  * `-p <port (int)>` The network port to listen to.
  * `-l logfile` Filename to use for logging.


# Contact

j.attema@esciencecenter.nl
