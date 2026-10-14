# HP LaserJet 1010 for macOS

Print to an HP LaserJet 1010 over USB from macOS applications. Requires an Apple Silicon Mac running macOS 26 or later.

## Download and install

Download the app from [Releases](https://github.com/adomasb/hp-laserjet-1010-macos/releases). Open **LaserJet 1010 Helper.app** and choose **Move to Applications** to install it and start using it.

## Build locally

Download or clone this repository, then run from the project folder:

```sh
scripts/build-app.sh
```

This produces `build/LaserJet 1010 Helper.app`. Open it and choose **Move to Applications** to install it and start using it.

## Print

Connect and power on the printer and open the app. It automatically adds the printer to macOS if it is missing. In Preview, Chrome, or another macOS app, open **File → Print** (or press **⌘P**) and choose **HP LaserJet 1010**. The local IPP service listens on `127.0.0.1:8632` and supports 600 dpi, monochrome, one-sided printing on A4 or Letter paper.

If setup fails, use **Retry Setup**. **Repair Printer Setup** is also available in the app’s menu.

The app remains in the menu bar when its window closes. Use the app controls to stop or restart printing, and enable **Open at login** to start it when you sign in.

## How it works

The app sends print jobs over local IPP, converts them to HP FastRaster, and sends them to the printer over USB; it uses no cloud services.

Run `scripts/test-all.sh` for the complete software checks and a validated local app ZIP. This runs without physical printing or system queue changes. See [local testing instructions](docs/testing.md) and [dependency notices and licenses](THIRD_PARTY_NOTICES.md).

## License

This project's original code is licensed under the [MIT License](LICENSE). Third-party components retain their respective licenses.
