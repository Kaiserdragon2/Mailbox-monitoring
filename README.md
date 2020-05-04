# Mailbox monitoring

On startup the application connects to the configured wlan access point.
After that it connects to the specified MQTT-Broker and publishes a message.
Then the ESP32 enters deepsleep.
If GPIO 2 or GPIO 4 get an High signal the ESP32 wakes up from deepsleep
and sends a message to an MQTT-Broker. Which message is send depends on the GPIO
that triggers the wakeup process.

## Hardware

Connect a button and a pulldown resistor to each of the two GPIOs.

