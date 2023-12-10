This project uses an ESP32 to control a bipolar stepper motor that moves a rail with a camera attached to it for focus bracketing of small things. It can be used with a macro lens or with a microscope objective attached to the camera.
Alternatively, you can mount the camera perpendicular to the rail for stuff like stop motion animation.
The code can remotely trigger a cellphone via built in BT keyboard, Canon, Panasonic and Olympus cameras via Wi-Fi and Canon and Nikon cameras by IR (although I didn't test it on Nikon cameras because I don't have one).
Panasonic and Olympus cameras are triggered via their webserver interfaces, and the Canons by TCP/IP. Sony, Nikon and Fujifilm cameras also have some implementation of TCP/IP, but I don't have a camera to test. For Canon I tested it with my Canon M3, but it should work with other cameras too.
Unfortunately, the PTP/IP protocol each camera uses is not documented very well, so many thing are camera or brand specific.

How to use
Hardware: 
You need a motorized/slider rail with a bipolar stepper motor, an ESP32 dev board, a stepper controller, and a power source for the controller. The motor I use is rated at 24v, but it works fine with an old laptop power brick that outputs 18V. If you want IR capabilities you will also need an IR LED and a 170 ohm resistor for the LED.
To put it all together, connect the stepper to the controller, the ctroller to the ESP32 and power, and optionally put an IR LED. The pins are specified in the source file, but they can be changed if needed. Once this is done, just glue or attach via some other method a tripod head on the rail, and you should be done.

Software:
This is a platformio project, so you need to download Visual Studio Code and then the Platformio plugin. You can also use Arduino if you want, you will have to rename the .cpp file as .ino
Once you uploaded the code, the ESP32 will start in AP mode. Connect your phone or the "esp32" wireless network. The password is "testtest" (you can change it in the source code if you want). Once connected, open the following address in your browser: http://192.168.4.1
In that page, click on Config SSID and enter the name and password of your Wi-Fi network. Or you can use the whole thing from the AP mode. Then go to your router admin page and figure out the IP of the esp32, and you can access it from your Wi-Fi network.
In the "Camera Config" page you can enter the camera IP address or the camera SSID/Password (for camera that don't want to connect to your Wi-Fi network, such as Olympus cameras). Canon and Panasonic can work both in AP mode or they can connect to your main network. Use them connected to your main network.
On that page you can also use the Test buttons to see if everything works (the camera should take a picture). For Canon, use "PTP Shoot".

If you wish to use your phone instead of a dedicated camera, then go to the BT settings and pair with the ESP keyboard. Put some macro lens on the phone, mount it on the rail, start your favorite camera app, set the fixed focus, and you are good to go.

This project is useful if you need to trigger cameras remotely for other projects (motion detection, etc.). Feel free to reuse the code, but if you do, please let me know. If you wish to extend this project to include other cameras, please let me know also. Just open a new issue.
If you have trouble getting it to work, have a suggestion or wish to tell me something, feel free to open an issue on Github, or e-mail me at radu.prv at gmail.com

Credits: https://julianschroden.com/ for some documentation of the PTP/IP protocol used by Canon cameras. I also borrowed some code from this project, but I had to modify it quite a bit to work: https://github.com/1stCLord/cptpip
I got the code for Canon and Nikon IR from some other projects, but unfortunately I can't find the source.

If you want to buy the hardware from Ali Express, here are the parts you can buy. make sure to check different sellers, and look at the shipping price too. Get the 5cm one with 1mm pitch. Make sure to select the right product. Some come with the drivers, but it's usually cheaper to buy the drivers separately.

Slider: https://www.aliexpress.com/item/1005006081438383.html

Driver: https://www.aliexpress.com/item/1005004712884586.html

ESP32: https://www.aliexpress.com/item/1005005704190069.html

Optional: https://www.aliexpress.com/item/1005004659870460.html? (get the x4 objective). If you do, and have a 3d printer, check this out: https://www.printables.com/model/143754-microscope-adapter-for-4x-macro-photography-with-s There is a dragonfly eye image I posted in the project taken with this setup (and an Olympus M10 Mk II), stacked in Helicon Focus.
