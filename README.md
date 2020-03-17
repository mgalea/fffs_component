# Fast Filing System Example

This example demonstrates how to use the Fast Filing Systems (FFS) on an SD card with ESP32.

FFS is a Write-Ahead filing system has been designed for data logging on solid state small storage systems such as SD cards. Its advantages are:
 - Extremely fast to store and retrieve data
 - Wear leveling built-in
 - Thread safe for concurrent read/write operations in a real time o/s environment
 - Atomic message indexing - messages can be retrieved at SD card sector level even if partition table is corrupted
 - Very compact-Messages can be as small as one byte which would take 2 bytes of storage space.
 - Very simple and small partition table - one block for every 256 blocks of SD card.

 Its disadvantages are: 
 - Messages have to be smaller than the sector size - 510 bytes for SD cards. this is normally sufficient for data logging purposes.
 - No packing to compact messages. Can result in huge storage space loss if messages are around 256 bytes long
 - No error-checking (future project)
 - No auto-healing  for corrupted partitons/sectors (future project)
