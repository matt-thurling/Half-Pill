Forked from https://github.com/gokuxmaker/Half-Pill-

Motivation for the fork was more for my own learning. Things I've changed:
* Renamed all references of "pills" to "meds". This is just so that if a reminder is for a medication that doesn't come in pill form then it will still make sense.
* Switched the storage of config and medicines to using the Preference library rather than EEPROM
* Implemented proper timezones - I live in a timezone with daylight savings so rather than needing to manually update the clock config twice a year, this should handle it
* Added in settings for screen brightness - I have this device on my bedside table and it lights up the whole room at night. I've introduced an idle brightness and and active brighness setting. Active brightness will be used if there is an active reminder or if the user has tapped the clock. Idle is used when its on the clock screen at all other times. Note that for this to work you'll need to switch the KE button on the display board to enable the backlight display functionality - see https://wiki.seeedstudio.com/seeedstudio_round_display_usage/#ke-button--gpio


Original project description:
====================================================
Missing medication schedules is more common than we think, especially for those with busy routines or elderly family members. I want to design something easy to use—a simple device that gently reminds users to take their medication at the right time.
So I built HALF PILL a smart Wi-Fi enabled Pill Reminder using the Seeed Studio XIAO ESP32-C3 and their beautiful Round Display for XIAO. The device connects to your home Wi-Fi, syncs time using NTP servers, and allows pill schedules to be configured directly from a modern mobile web interface.
All pill schedules are configured through a mobile web interface that can be accessed from any phone connected to the same Wi-Fi network. The interface allows users to view existing schedules and add new medications easily, while the ESP32 stores the data securely in EEPROM.
The user can schedule up to 20 pills using this device
So let's go build one
https://www.hackster.io/Gokux/half-pill-a-minimal-smart-desk-pill-reminder-34f8f5
