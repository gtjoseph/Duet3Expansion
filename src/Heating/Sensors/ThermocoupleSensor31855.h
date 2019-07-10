/*
 * ThermocoupleSensor31855.h
 *
 *  Created on: 8 Jun 2017
 *      Author: David
 */

#ifndef SRC_HEATING_THERMOCOUPLESENSOR31855_H_
#define SRC_HEATING_THERMOCOUPLESENSOR31855_H_

#include "SpiTemperatureSensor.h"

#if SUPPORT_SPI_SENSORS

class ThermocoupleSensor31855 : public SpiTemperatureSensor
{
public:
	ThermocoupleSensor31855(unsigned int channel);
	void Init() override;

protected:
	TemperatureError TryGetTemperature(float& t) override;
};

#endif

#endif /* SRC_HEATING_THERMOCOUPLESENSOR31855_H_ */