Simple demo of writing Linux device drivers and kernel modules.
In this exercise the goal was to write a pseudo char device representing slots for inter-proccess communication.
Each slot consists of an unbounded number of channels, each can contain a message of up to 128 Bytes.
