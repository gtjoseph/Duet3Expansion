/*
 * AnalogIn.cpp
 *
 *  Created on: 6 Sep 2018
 *      Author: David
 */

#include "AnalogIn.h"
#include "RTOSIface/RTOSIface.h"
#include "Hardware/DmacManager.h"
#include "Hardware/IoPorts.h"

static uint32_t conversionsStarted = 0;
static uint32_t conversionsCompleted = 0;

class AdcClass
{
public:
	enum class State : uint8_t
	{
		noChannels = 0,
		starting,
		idle,
		converting,
		ready
	};

	AdcClass(Adc * const p_device, IRQn p_irqn, DmaChannel p_dmaChan, DmaTrigSource p_trigSrc);

	State GetState() const { return state; }
	bool EnableChannel(unsigned int chan, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t p_ticksPerCall);
	bool IsChannelEnabled(unsigned int chan);
	bool EnableTemperatureSensor(unsigned int sensorNumber, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t ticksPerCall);
	bool StartConversion(TaskBase *p_taskToWake);
	uint16_t ReadChannel(unsigned int chan) const { return resultsByChannel[chan]; }

	void ResultReadyCallback();
	void ExecuteCallbacks();

private:
	bool InternalEnableChannel(unsigned int chan, uint8_t refCtrl, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t p_ticksPerCall);
	size_t GetChannel(size_t slot) { return inputRegisters[2 * slot] & 0x1F; }

	static void DmaCompleteCallback(CallbackParameter cp);

	static constexpr size_t NumAdcChannels = 32;		// number of channels per ADC including temperature sensor inputs etc.
	static constexpr size_t MaxSequenceLength = 16;		// the maximum length of the read sequence

	Adc * const device;
	const IRQn irqn;
	const DmaChannel dmaChan;
	const DmaTrigSource trigSrc;

	size_t numChannelsEnabled;
	uint32_t channelsEnabled;
	TaskBase *taskToWake;
	State state;
	bool justStarted;
	AnalogInCallbackFunction callbackFunctions[MaxSequenceLength];
	CallbackParameter callbackParams[MaxSequenceLength];
	uint32_t ticksPerCall[MaxSequenceLength];
	uint32_t ticksAtLastCall[MaxSequenceLength];
	uint32_t inputRegisters[MaxSequenceLength * 2];
	volatile uint16_t results[MaxSequenceLength];
	volatile uint16_t resultsByChannel[32];		// must be large enough to handle PTAT and CTAT temperature sensor inputs
};

AdcClass::AdcClass(Adc * const p_device, IRQn p_irqn, DmaChannel p_dmaChan, DmaTrigSource p_trigSrc)
	: device(p_device), irqn(p_irqn), dmaChan(p_dmaChan), trigSrc(p_trigSrc),
	  numChannelsEnabled(0), channelsEnabled(0), taskToWake(nullptr), state(State::noChannels)
{
	for (size_t i = 0; i < MaxSequenceLength; ++i)
	{
		callbackFunctions[i] = nullptr;
		callbackParams[i].u32 = 0;
		resultsByChannel[i] = 0;
	}
}

// Try to enable this ADC on the specified pin returning true if successful
// Only single ended mode with gain x1 is supported
// There is no check to avoid adding the same channel twice. If you do that it will be converted twice.
bool AdcClass::EnableChannel(unsigned int chan, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t p_ticksPerCall)
{
	if (numChannelsEnabled == MaxSequenceLength || chan >= MaxSequenceLength)
	{
		return false;
	}

	return InternalEnableChannel(chan, ADC_REFCTRL_REFSEL_INTVCC1, fn, param, p_ticksPerCall);
}

bool AdcClass::IsChannelEnabled(unsigned int chan)
{
	return (channelsEnabled & (1ul << chan)) != 0;
}

bool AdcClass::EnableTemperatureSensor(unsigned int sensorNumber, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t p_ticksPerCall)
{
	if (numChannelsEnabled == MaxSequenceLength || sensorNumber >= 2)
	{
		return false;
	}

//	return InternalEnableChannel(sensorNumber + ADC_INPUTCTRL_MUXPOS_PTAT_Val, ADC_REFCTRL_REFSEL_INTREF, fn, param, p_ticksPerCall);
	return InternalEnableChannel(sensorNumber + ADC_INPUTCTRL_MUXPOS_PTAT_Val, ADC_REFCTRL_REFSEL_INTVCC1, fn, param, p_ticksPerCall);
}

bool AdcClass::InternalEnableChannel(unsigned int chan, uint8_t refCtrl, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t p_ticksPerCall)
{
	if (chan < 32)
	{
		TaskCriticalSectionLocker lock;

		// Set up the ADC
		callbackFunctions[numChannelsEnabled] = fn;
		callbackParams[numChannelsEnabled] = param;
		ticksPerCall[numChannelsEnabled] = p_ticksPerCall;
		ticksAtLastCall[numChannelsEnabled] = millis();
		inputRegisters[numChannelsEnabled * 2] = ADC_INPUTCTRL_MUXNEG_GND | (uint32_t)chan;
		inputRegisters[numChannelsEnabled * 2 + 1] = refCtrl;
		resultsByChannel[chan] = 0;
		++numChannelsEnabled;
		channelsEnabled |= 1ul << chan;

		if (numChannelsEnabled == 1)
		{
			// First channel is being enabled
			// Initialise the ADC
			if (!hri_adc_is_syncing(device, ADC_SYNCBUSY_SWRST))
			{
				if (hri_adc_get_CTRLA_reg(device, ADC_CTRLA_ENABLE))
				{
					hri_adc_clear_CTRLA_ENABLE_bit(device);
					hri_adc_wait_for_sync(device, ADC_SYNCBUSY_ENABLE);
				}
				hri_adc_write_CTRLA_reg(device, ADC_CTRLA_SWRST);
			}
			hri_adc_wait_for_sync(device, ADC_SYNCBUSY_SWRST);

			hri_adc_write_CTRLA_reg(device, ADC_CTRLA_PRESCALER_DIV32);
			hri_adc_write_CTRLB_reg(device, 0);
			hri_adc_write_REFCTRL_reg(device,  ADC_REFCTRL_REFSEL_INTVCC1);
			hri_adc_write_EVCTRL_reg(device, ADC_EVCTRL_RESRDYEO);
			hri_adc_write_INPUTCTRL_reg(device, ADC_INPUTCTRL_MUXNEG_GND);
			hri_adc_write_AVGCTRL_reg(device, 0);
//			hri_adc_write_SAMPCTRL_reg(device, ADC_SAMPCTRL_OFFCOMP);		// this also extends the sample time
			hri_adc_write_SAMPCTRL_reg(device, ADC_SAMPCTRL_SAMPLEN(10));
			hri_adc_write_WINLT_reg(device, 0);
			hri_adc_write_WINUT_reg(device, 0xFFFF);
			hri_adc_write_GAINCORR_reg(device, 1u << 11);
			hri_adc_write_OFFSETCORR_reg(device, 0);
			hri_adc_write_DBGCTRL_reg(device, 0);

			// Enable DMA sequencing, updating just the input and reference control registers.
			// We have to set the AUTOSTART bit too, otherwise the ADC requires one trigger per channel converted.
			hri_adc_write_DSEQCTRL_reg(device, ADC_DSEQCTRL_INPUTCTRL | ADC_DSEQCTRL_REFCTRL | ADC_DSEQCTRL_AUTOSTART);
			hri_adc_set_CTRLA_ENABLE_bit(device);

			// Set the supply controller to on-demand mode so that we can get at both temperature sensors
			hri_supc_set_VREF_ONDEMAND_bit(SUPC);
			hri_supc_set_VREF_TSEN_bit(SUPC);
			hri_supc_clear_VREF_VREFOE_bit(SUPC);

			// Initialise the DMAC. First the sequencer
			DmacSetDestinationAddress(dmaChan, &device->DSEQDATA.reg);
			DmacSetBtctrl(dmaChan, DMAC_BTCTRL_VALID | DMAC_BTCTRL_EVOSEL_DISABLE | DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_BEATSIZE_WORD
									| DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_STEPSIZE_X1);
#if defined(SAME51)
			DMAC->Channel[dmaChan].CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC((uint8_t)trigSrc + 1) | DMAC_CHCTRLA_TRIGACT_BURST
												| DMAC_CHCTRLA_BURSTLEN_SINGLE | DMAC_CHCTRLA_THRESHOLD_1BEAT;
#elif defined(SAMC21)
			DMAC->CHID.reg = dmaChan;
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC((uint8_t)trigSrc + 1) | DMAC_CHCTRLA_TRIGACT_BURST
												| DMAC_CHCTRLA_BURSTLEN_SINGLE | DMAC_CHCTRLA_THRESHOLD_1BEAT;
#else
# error Unsupported processor
#endif

			// Now the result reader
			DmacSetSourceAddress(dmaChan + 1, const_cast<uint16_t *>(&device->RESULT.reg));
			DmacSetInterruptCallbacks(dmaChan + 1, DmaCompleteCallback, nullptr, this);
			DmacSetBtctrl(dmaChan + 1, DMAC_BTCTRL_VALID | DMAC_BTCTRL_EVOSEL_DISABLE | DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_BEATSIZE_HWORD
										| DMAC_BTCTRL_DSTINC | DMAC_BTCTRL_STEPSEL_DST | DMAC_BTCTRL_STEPSIZE_X1);
#if defined(SAME51)
			DMAC->Channel[dmaChan + 1].CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC((uint8_t)trigSrc) | DMAC_CHCTRLA_TRIGACT_BURST
													| DMAC_CHCTRLA_BURSTLEN_SINGLE | DMAC_CHCTRLA_THRESHOLD_1BEAT;
#elif defined(SAMC21)
			DMAC->CHID.reg = dmaChan + 1;
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_TRIGSRC((uint8_t)trigSrc) | DMAC_CHCTRLA_TRIGACT_BURST
													| DMAC_CHCTRLA_BURSTLEN_SINGLE | DMAC_CHCTRLA_THRESHOLD_1BEAT;
#else
# error Unsupported processor
#endif
			state = State::starting;
		}

		return true;
	}

	return false;
}

bool AdcClass::StartConversion(TaskBase *p_taskToWake)
{
	if (numChannelsEnabled == 0 || state == State::converting)
	{
		return false;
	}

	taskToWake = p_taskToWake;
	(void)device->RESULT.reg;			// make sure no result pending (this is necessary to make it work!)

	// Set up DMA to read the results our of the ADC into the results array
	DmacSetDestinationAddress(dmaChan + 1, results);
	DmacSetDataLength(dmaChan + 1, numChannelsEnabled);
	DmacEnableCompletedInterrupt(dmaChan + 1);
	DmacEnableChannel(dmaChan + 1);

	DmacSetSourceAddress(dmaChan, inputRegisters);
	DmacSetDataLength(dmaChan, numChannelsEnabled * 2);
	DmacEnableChannel(dmaChan);

	state = State::converting;
	++conversionsStarted;
	return true;
}

void AdcClass::ExecuteCallbacks()
{
	TaskCriticalSectionLocker lock;
	const uint32_t now = millis();
	for (size_t i = 0; i < numChannelsEnabled; ++i)
	{
		resultsByChannel[GetChannel(i)] = results[i];
		if (now - ticksAtLastCall[i] >= ticksPerCall[i])
		{
			ticksAtLastCall[i] = now;
			if (callbackFunctions[i] != nullptr)
			{
				callbackFunctions[i](callbackParams[i], results[i]);
			}
		}
	}
}

// Indirect callback from the DMA controller ISR
void AdcClass::ResultReadyCallback()
{
	state = State::ready;
	++conversionsCompleted;
	DmacDisableChannel(dmaChan);			// disable the sequencer DMA, just in case it is out of sync
	if (taskToWake != nullptr)
	{
		taskToWake->GiveFromISR();
	}
}

// Callback from the DMA controller ISR
/*static*/ void AdcClass::DmaCompleteCallback(CallbackParameter cp)
{
	static_cast<AdcClass *>(cp.vp)->ResultReadyCallback();
}

// ADC instances
static AdcClass Adcs[] =
{
#if defined(SAME51)
	AdcClass(ADC0, ADC0_0_IRQn, Adc0TxDmaChannel, DmaTrigSource::adc0_resrdy),
	AdcClass(ADC1, ADC1_0_IRQn, Adc1TxDmaChannel, DmaTrigSource::adc1_resrdy)
#elif defined(SAMC21)
	// We use onoy the first ADC
	AdcClass(ADC0, ADC0_IRQn, Adc0TxDmaChannel, DmaTrigSource::adc0_resrdy),
#else
# error Unsupported processor
#endif
};

namespace AnalogIn
{
	// Analog input management task
	constexpr size_t AnalogInTaskStackWords = 200;
	static Task<AnalogInTaskStackWords> analogInTask;

	// Main loop executed by the AIN task
	extern "C" void AinLoop(void *)
	{
		// Loop taking readings and processing them
		for (;;)
		{
			// Loop through ADCs
			bool conversionStarted = false;
			for (size_t i = 0; i < ARRAY_SIZE(Adcs); ++i)
			{
				AdcClass& adc = Adcs[i];
				switch (adc.GetState())
				{
				case AdcClass::State::ready:
					adc.ExecuteCallbacks();
					//no break
				case AdcClass::State::idle:
				case AdcClass::State::starting:
					adc.StartConversion(&analogInTask);
					conversionStarted = true;
					break;

				default:	// no channels enabled, or conversion in progress
					break;
				}
			}

			if (conversionStarted)
			{
				if (TaskBase::Take(500) == 0)
				{
					//TODO we had a timeout so record an error
				}
				delay(2);
			}
			else
			{
				// No ADCs enabled yet, or all converting
				delay(10);
			}
		}
	}
}

// Initialise the analog input subsystem. Call this just once.
void AnalogIn::Init()
{
	// Enable ADC clocks
#if defined(SAME51)
	hri_mclk_set_APBDMASK_ADC0_bit(MCLK);
	hri_gclk_write_PCHCTRL_reg(GCLK, ADC0_GCLK_ID, GCLK_PCHCTRL_GEN_GCLK0_Val | (1 << GCLK_PCHCTRL_CHEN_Pos));
	hri_mclk_set_APBDMASK_ADC1_bit(MCLK);
	hri_gclk_write_PCHCTRL_reg(GCLK, ADC1_GCLK_ID, GCLK_PCHCTRL_GEN_GCLK0_Val | (1 << GCLK_PCHCTRL_CHEN_Pos));
#elif defined(SAMC21)
	// SAMC21 has 2 ADCs but we use only the first one
	hri_mclk_set_APBCMASK_ADC0_bit(MCLK);
	hri_gclk_write_PCHCTRL_reg(GCLK, ADC0_GCLK_ID, GCLK_PCHCTRL_GEN_GCLK0_Val | (1 << GCLK_PCHCTRL_CHEN_Pos));
#else
# error Unsupported processor
#endif

#if 0
	// Set the supply controller to on-demand mode so that we can get at both temperature sensors
	hri_supc_set_VREF_ONDEMAND_bit(SUPC);
	hri_supc_set_VREF_TSEN_bit(SUPC);
	hri_supc_clear_VREF_VREFOE_bit(SUPC);
#endif

	analogInTask.Create(AinLoop, "AIN", nullptr, TaskPriority::AinPriority);
}

// Enable analog input on a pin.
// Readings will be taken and about every 'ticksPerCall' milliseconds the callback function will be called with the specified parameter and ADC reading.
// Set ticksPerCall to 0 to get a callback on every reading.
bool AnalogIn::EnableChannel(Pin pin, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t ticksPerCall)
{
	if (pin < ARRAY_SIZE(PinTable))
	{
		const AdcInput adcin = IoPort::PinToAdcInput(pin);
		if (adcin != AdcInput::none)
		{
			IoPort::SetPinMode(pin, AIN);
			return Adcs[GetDeviceNumber(adcin)].EnableChannel(GetInputNumber(adcin), fn, param, ticksPerCall);
		}
	}
	return false;
}

// Return whether or not the channel is enabled
bool AnalogIn::IsChannelEnabled(Pin pin)
{
	if (pin < ARRAY_SIZE(PinTable))
	{
		const AdcInput adcin = IoPort::PinToAdcInput(pin);
		if (adcin != AdcInput::none)
		{
			return Adcs[GetDeviceNumber(adcin)].IsChannelEnabled(GetInputNumber(adcin));
		}
	}
	return false;
}

#if 0
// Disable a previously-enabled channel
bool AnalogIn::DisableChannel(Pin pin)
{
	//TODO not implemented yet (do we need it?)
	return false;
}
#endif

uint16_t AnalogIn::ReadChannel(AdcInput adcin)
{
	return (adcin != AdcInput::none) ? Adcs[GetDeviceNumber(adcin)].ReadChannel(GetInputNumber(adcin)) : 0;
}

// Enable an on-chip MCU temperature sensor
bool AnalogIn::EnableTemperatureSensor(unsigned int sensorNumber, AnalogInCallbackFunction fn, CallbackParameter param, uint32_t ticksPerCall, unsigned int adcnum)
{
	if (adcnum < ARRAY_SIZE(Adcs))
	{
		return Adcs[adcnum].EnableTemperatureSensor(sensorNumber, fn, param, ticksPerCall);
	}
	return false;
}

void AnalogIn::GetDebugInfo(uint32_t &convsStarted, uint32_t &convsCompleted)
{
	convsStarted = conversionsStarted;
	convsCompleted = conversionsCompleted;
}

// End
