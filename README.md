# smartmet-plugin-frontend

Part of [SmartMet Server](https://github.com/fmidev/smartmet-server). See the [SmartMet Server documentation](https://github.com/fmidev/smartmet-server) for a full overview of the ecosystem.

## Overview

The frontend plugin implements the load-balancing frontend in a SmartMet Server cluster. It receives incoming requests and distributes them across backend servers discovered via [smartmet-engine-sputnik](https://github.com/fmidev/smartmet-engine-sputnik).

## Features

- Automatic backend discovery via Sputnik UDP broadcasting
- Request routing and load balancing across backend servers
- High-availability cluster support

## License

MIT — see [LICENSE](LICENSE)

## Contributing

Bug reports and pull requests are welcome on [GitHub](../../issues).
