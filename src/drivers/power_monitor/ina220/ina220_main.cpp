/****************************************************************************
 *
 *   Copyright (C) 2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
#include <px4_platform_common/getopt.h>
#include <px4_platform_common/module.h>

#include "ina220.h"

I2CSPIDriverBase *INA220::instantiate(const I2CSPIDriverConfig &config, int runtime_instance)
{
	INA220 *instance = new INA220(config, config.custom1);

	if (instance == nullptr) {
		PX4_ERR("alloc failed");
		return nullptr;
	}

	if (config.keep_running) {
		if (instance->force_init() != PX4_OK) {
			PX4_INFO("Failed to init INA220 on bus %d, but will try again periodically.", config.bus);
		}

	} else if (instance->init() != PX4_OK) {
		delete instance;
		return nullptr;
	}

	return instance;
}

void
INA220::print_usage()
{
	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
### Description
Driver for the INA220 power monitor.

Multiple instances of this driver can run simultaneously, if each instance has a separate bus OR I2C address.

For example, one instance can run on Bus 2, address 0x41, and one can run on Bus 2, address 0x43.

If the INA220 module is not powered, then by default, initialization of the driver will fail. To change this, use
the -f flag. If this flag is set, then if initialization fails, the driver will keep trying to initialize again
every 0.5 seconds. With this flag set, you can plug in a battery after the driver starts, and it will work. Without
this flag set, the battery must be plugged in before starting the driver.

)DESCR_STR");

	PRINT_MODULE_USAGE_NAME("ina220", "driver");

	PRINT_MODULE_USAGE_COMMAND("start");
	PRINT_MODULE_USAGE_PARAMS_I2C_SPI_DRIVER(true, false);
	PRINT_MODULE_USAGE_PARAMS_I2C_ADDRESS(0x41);
	PRINT_MODULE_USAGE_PARAMS_I2C_KEEP_RUNNING_FLAG();
	PRINT_MODULE_USAGE_PARAM_INT('t', 1, 1, 3, "battery index for calibration values (1 or 3)", true);
	PRINT_MODULE_USAGE_PARAM_STRING('T', "VBATT", "VBATT|VREG", "Type", true);
	PRINT_MODULE_USAGE_DEFAULT_COMMANDS();
}

extern "C" int
ina220_main(int argc, char *argv[])
{
	int ch;
	using ThisDriver = INA220;
	BusCLIArguments cli{true, false};
	cli.i2c_address = INA220_BASEADDR;
	cli.default_i2c_frequency = 100000;
	cli.support_keep_running = true;
	cli.custom1 = 1;

	cli.custom2 = PM_CH_TYPE_VBATT;
	while ((ch = cli.getOpt(argc, argv, "T:")) != EOF) {
		switch (ch) {
		case 't': // battery index
			cli.custom1 = (int)strtol(cli.optArg(), NULL, 0);
			break;
		case 'T':
			if (strcmp(cli.optArg(), "VBATT") == 0) {
				cli.custom2 = PM_CH_TYPE_VBATT;

			} else if (strcmp(cli.optArg(), "VREG") == 0) {
				cli.custom2 = PM_CH_TYPE_VREG;

			} else {
				PX4_ERR("unknown type");
				return -1;
			}

			break;
		}
	}

	const char *verb = cli.optArg();
	if (!verb) {
		ThisDriver::print_usage();
		return -1;
	}

	BusInstanceIterator iterator(MODULE_NAME, cli, DRV_POWER_DEVTYPE_INA220);

	if (!strcmp(verb, "start")) {
		return ThisDriver::module_start(cli, iterator);
	}

	if (!strcmp(verb, "stop")) {
		return ThisDriver::module_stop(iterator);
	}

	if (!strcmp(verb, "status")) {
		return ThisDriver::module_status(iterator);
	}

	ThisDriver::print_usage();
	return -1;
}
