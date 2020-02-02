/**
  ******************************************************************************
  * @file           : analog.c
  * @brief          : Analog axis driver implementation
  ******************************************************************************
  */

#include "analog.h"
#include <string.h>
#include <math.h>
#include "sensors.h"

analog_data_t input_data[MAX_AXIS_NUM];

analog_data_t scaled_axis_data[MAX_AXIS_NUM];
analog_data_t raw_axis_data[MAX_AXIS_NUM];
analog_data_t out_axis_data[MAX_AXIS_NUM];

analog_data_t FILTER_LOW_COEFF[FILTER_LOW_SIZE] = {40, 30, 15, 10, 5};
analog_data_t FILTER_MED_COEFF[FILTER_MED_SIZE] = {30, 20, 10, 10, 10, 6, 6, 4, 2, 2};
analog_data_t FILTER_HIGH_COEFF[FILTER_HIGH_SIZE] = {20, 20, 10, 10, 5, 5, 5, 5, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1};

analog_data_t filter_buffer[MAX_AXIS_NUM][FILTER_HIGH_SIZE];
	
uint32_t err_cnt = 0;

adc_channel_config_t channel_config[MAX_AXIS_NUM] =
{
	{ADC_Channel_0, 0}, {ADC_Channel_1, 1}, 
	{ADC_Channel_2, 2}, {ADC_Channel_3, 3},
	{ADC_Channel_4, 4}, {ADC_Channel_5, 5}, 
	{ADC_Channel_6, 6}, {ADC_Channel_7, 7}, 
};

///**
//  * @brief 	Returns absolute value of input parameter
//	*	@param	x: Input value
//  * @retval Absolute value
//  */
//static int32_t abs (int32_t x)
//{
//	return (x > 0) ? x : -x;
//}

/**
  * @brief  Transform value from input range to value in output range
	*	@param	x: Value to transform
	*	@param	in_min:	Minimum value of input range
	*	@param	in_max:	Maximum value of input range
	*	@param	out_min: Minimum value of output range
	*	@param	out_max: Maximum value of output range
  * @retval Transformed value
  */
static int32_t map2(	int32_t x, 
											int32_t in_min, 
											int32_t in_max, 
											int32_t out_min,
											int32_t out_max)
{
	int32_t tmp;
	int32_t ret;
	
	tmp = x;
	
	
	if (tmp < in_min)	return out_min;
	if (tmp > in_max)	return out_max;
		
	ret = (tmp - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
	
	return ret;
}

/**
  * @brief  Transform value of input range -180000 to 180000 to range -32767 to 32767
	*	@param	x: Value to transform
  * @retval Transformed value
  */
static int32_t map_tle (int32_t x)
{
	int32_t tmp;
	int32_t ret;
	
	tmp = x+180;
	
	ret = tmp * 100 / 549 - 32767;
	
	return ret;
}

/**
  * @brief  Transform value from input range to value in output range 
	*	@param	x: Value to transform
	*	@param	in_min:	Minimum value of input range
	*	@param	in_center: Center value of input range
	*	@param	in_max:	Maximum value of input range
	*	@param	out_min: Minimum value of output range
	*	@param	out_center:	Center value of input range
	*	@param	out_max: Maximum value of output range
	*	@param	dead_zone: Width of center dead zone
  * @retval Transformed value
  */
static int32_t map3(	int32_t x, 
											int32_t in_min, 
											int32_t in_center, 
											int32_t in_max, 
											int32_t out_min,
											int32_t out_center,
											int32_t out_max,
											uint8_t dead_zone)
{
	int32_t tmp;
	int32_t ret;
	int32_t dead_zone_right;
	int32_t dead_zone_left;
	
	tmp = x;
	dead_zone_right = ((in_max - in_center)*dead_zone)>>10;
	dead_zone_left = ((in_center - in_min)*dead_zone)>>10;
	
	if (tmp < in_min)	return out_min;
	if (tmp > in_max)	return out_max; 
	if ((tmp > in_center && (tmp - in_center) < dead_zone_right) || 
			(tmp < in_center &&	(in_center - tmp) < dead_zone_left))
	{
		return in_center;
	}		
	
	if (tmp < in_center)
	{
		ret = ((tmp - in_min) * (out_center - out_min) / (in_center - dead_zone_left - in_min) + out_min);
  }
	else
	{
		ret = ((tmp - in_center - dead_zone_right) * (out_max - out_center) / (in_max - in_center - dead_zone_right) + out_center);
	}
	return ret;
}

/**
  * @brief  Lowing input data resolution
	*	@param	value: Value to process
	*	@param	resolution:	Desired resolution of value in bits
  * @retval Resulting value
  */
analog_data_t SetResolutioin (analog_data_t value, uint8_t resolution)
{
	int32_t tmp = 0;
	int32_t ret = 0;
	uint32_t fullscale = AXIS_MAX_VALUE - AXIS_MIN_VALUE;
	float step;
	
	if (resolution >= 16)
	{
		return value;
	}
	else if (resolution > 0)
	{
		tmp = fullscale >> (16 - resolution);
		step = (float)fullscale/tmp;
		
		tmp = value;
		tmp = (tmp - AXIS_MIN_VALUE)  >> (16 - resolution);
		ret = step * tmp + AXIS_MIN_VALUE;
	}
	
	return ret;
}

/**
  * @brief  FIR filter for input data
	*	@param	value: Value to process
	*	@param	filter_buf:	Pointer to filter data buffer
	*	@param	filter_lvl:	Desired filter level
	*   This parameter can be 0-3 (where 0 is no filtration, 3 is high filtration level)
  * @retval Resulting value
  */
analog_data_t Filter (analog_data_t value, analog_data_t * filter_buf, filter_t filter_lvl)
{
	int32_t tmp32;
	
	switch (filter_lvl)
	{
		default:
		case FILTER_NO:
			return value;
		
		case FILTER_LOW:
			tmp32 = value * FILTER_LOW_COEFF[0];
			for (uint8_t i=FILTER_LOW_SIZE-1; i>0; i--)
			{
				filter_buf[i] = filter_buf[i-1];
				
				tmp32 += filter_buf[i] * FILTER_LOW_COEFF[i];
			}
		break;
		
		case FILTER_MEDIUM:
			tmp32 = value * FILTER_MED_COEFF[0];
			for (uint8_t i=FILTER_MED_SIZE-1; i>0; i--)
			{
				filter_buf[i] = filter_buf[i-1];
				
				tmp32 += filter_buf[i] * FILTER_MED_COEFF[i];
			}
		break;
		
		case FILTER_HIGH:
			tmp32 = value * FILTER_HIGH_COEFF[0];
			for (uint8_t i=FILTER_HIGH_SIZE-1; i>0; i--)
			{
				filter_buf[i] = filter_buf[i-1];
				
				tmp32 += filter_buf[i] * FILTER_HIGH_COEFF[i];
			}
			
		break;
	}
	
	filter_buf[0] = (uint16_t)(tmp32/100);
	
	
	return filter_buf[0];
}

/**
  * @brief  Scaling input data accodring to set axis curve shape
	*	@param	p_axis_cfg: Pointer to axis configuration structure
	*	@param	value:	Value to process
	*	@param	point_cnt:	Number of points in axis curve
  * @retval Resulting value
  */
analog_data_t ShapeFunc (axis_config_t * p_axis_cfg,  analog_data_t value, uint8_t point_cnt)
{
	int32_t out_min, out_max, step;
	int32_t in_min, in_max;
	uint8_t min_index;
	analog_data_t ret;
	
	int32_t tmp = value - AXIS_MIN_VALUE;
	int32_t fullscale = AXIS_MAX_VALUE - AXIS_MIN_VALUE;
	
	step = (float)fullscale/((float)point_cnt-1.0f);
	min_index = tmp/step;
	
	if (min_index == point_cnt-1) min_index = point_cnt-2;
	
	in_min = AXIS_MIN_VALUE + min_index*step;
	in_max = AXIS_MIN_VALUE + (min_index+1)*step;
	
	out_min = ((int32_t)p_axis_cfg->curve_shape[min_index] * (int32_t)fullscale/200 + (int32_t)AXIS_CENTER_VALUE);
	out_max = ((int32_t)p_axis_cfg->curve_shape[min_index+1] * (int32_t)fullscale/200 + (int32_t)AXIS_CENTER_VALUE);
	
	ret = map2(value, in_min, in_max, out_min, out_max);
	
	return(ret);
}

/**
  * @brief  Axes initialization after startup
	*	@param	p_config: Pointer to device configuration structure
  * @retval None
  */
void AxesInit (app_config_t * p_config)
{
	uint8_t adc_cnt = 0;
	uint8_t sensors_cnt = 0;
	
  ADC_InitTypeDef ADC_InitStructure;
	DMA_InitTypeDef DMA_InitStructure;

	 /* DMA and ADC controller clock enable */
  RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1 | RCC_APB2Periph_GPIOC, ENABLE);
	
	
	// Count ADC channels
	for (int i=0; i<USED_PINS_NUM; i++)
	{
		if (p_config->pins[i] == AXIS_ANALOG)
		{
			adc_cnt++;
		}
		else if (p_config->pins[i] == TLE5011_CS)
		{
			sensors_cnt++;
		}
	}
	
	if ((adc_cnt + sensors_cnt) > MAX_AXIS_NUM)
	{
		// Error
		adc_cnt = 0;
		sensors_cnt = 0;
	}
	
	// Init ADC
	if (adc_cnt > 0)
	{
		/* ADC1 configuration ------------------------------------------------------*/
		ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
		ADC_InitStructure.ADC_ScanConvMode = ENABLE;
		ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;
		ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
		ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
		ADC_InitStructure.ADC_NbrOfChannel = adc_cnt;
		ADC_Init(ADC1, &ADC_InitStructure);

		/* Enable ADC1 DMA */
		ADC_DMACmd(ADC1, ENABLE);
	}
	
	uint8_t axis_num = 0;
	for (int i=0; i<USED_PINS_NUM; i++)
	{
		// Configure Sensors channels		
		if (p_config->pins[i] == TLE5011_CS)
		{
			axis_num++;
		}
	}
	for (int i=0; i<MAX_AXIS_NUM; i++)
	{ 
		if (p_config->pins[i] == AXIS_ANALOG)		// Configure ADC channels
		{
			/* ADC1 regular channel configuration */ 
			ADC_RegularChannelConfig(ADC1, channel_config[i].channel, i, ADC_SampleTime_239Cycles5);
			axis_num++;
		}
		
	}

	if (adc_cnt > 0)
	{
		/* DMA1 channel1 configuration ----------------------------------------------*/
		DMA_DeInit(DMA1_Channel1);
		DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR;
		DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t) &input_data[0];
		DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
		DMA_InitStructure.DMA_BufferSize = adc_cnt;
		DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
		DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
		DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
		DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
		DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
		DMA_InitStructure.DMA_Priority = DMA_Priority_High;
		DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
		DMA_Init(DMA1_Channel1, &DMA_InitStructure);
			
			/* Enable DMA1 channel1 */
		DMA_Cmd(DMA1_Channel1, ENABLE);
			/* Enable ADC1 */
		ADC_Cmd(ADC1, ENABLE);
			
			/* Enable ADC1 reset calibration register */   
		ADC_ResetCalibration(ADC1);
		/* Check the end of ADC1 reset calibration register */
		while(ADC_GetResetCalibrationStatus(ADC1));

		/* Start ADC1 calibration */
		ADC_StartCalibration(ADC1);
		/* Check the end of ADC1 calibration */
		while(ADC_GetCalibrationStatus(ADC1));
			 
		/* Start ADC1 Software Conversion */ 
		ADC_SoftwareStartConvCmd(ADC1, ENABLE);
	}
}

/**
  * @brief  Axes data processing routine
	*	@param	p_config: Pointer to device configuration structure
  * @retval None
  */
void AxesProcess (app_config_t * p_config)
{
	int32_t tmp[MAX_AXIS_NUM];
	float tmpf;
	uint8_t analog_channel = 0;
	
	for (uint8_t i=0; i<MAX_AXIS_NUM; i++)
	{
		
		int8_t source = p_config->axis_config[i].source_main;
		
		if (source >= 0)
		{
			// source TLE501x
			if (p_config->pins[source] == TLE5011_CS)
			{
				tmpf = 0;
				if (TLE501x_Get(&pin_config[source], &tmpf) == 0)
				{
					if (p_config->axis_config[i].magnet_offset)
					{
						tmpf -= 180;
						if (tmpf < -180) tmpf += 360;
						else if (tmpf > 180) tmpf -= 360;
					}
					tmpf *= 1000;
					//raw_axis_data[channel] = map2(tmpf, -180000, 180000, AXIS_MIN_VALUE, AXIS_MAX_VALUE);
					raw_axis_data[i] = map_tle(tmpf);
				}
				else
				{
					err_cnt++;
				}
			}
			// source analog
			else if (p_config->pins[source] == AXIS_ANALOG)
			{
				if (p_config->axis_config[i].magnet_offset)
				{
						tmp[i] = input_data[analog_channel++] - 2047;
						if (tmp < 0) tmp[i] += 4095;
						else if (tmp[i] > 4095) tmp[i] -= 4095;
				}
				else
				{
					tmp[i] = input_data[analog_channel++];
				}
				
				raw_axis_data[i] = map2(tmp[i], 0, 4095, AXIS_MIN_VALUE, AXIS_MAX_VALUE);
			}
		}
		
		// Filtering
		tmp[i] = Filter(raw_axis_data[i], filter_buffer[i], p_config->axis_config[i].filter);
			
    // Scale output data
    tmp[i] = map3( tmp[i], 
                 p_config->axis_config[i].calib_min,
                 p_config->axis_config[i].calib_center,    
                 p_config->axis_config[i].calib_max, 
                 AXIS_MIN_VALUE,
                 AXIS_CENTER_VALUE,
                 AXIS_MAX_VALUE,
                 p_config->axis_config[i].dead_zone);    
    // Shaping
    tmp[i] = ShapeFunc(&p_config->axis_config[i], tmp[i], 11);
    // Lowing resolution if needed
    tmp[i] = SetResolutioin(tmp[i], p_config->axis_config[i].resolution);
    
    // Invertion
    if (p_config->axis_config[i].inverted > 0)
    {
      tmp[i] = 0 - tmp[i];
    }
	}
      
	for (uint8_t i=0; i<MAX_AXIS_NUM; i++)
	{
		// Multi-axis process
		if (p_config->axis_config[i].function != NO_FUNCTION)
		{
			{
				switch (p_config->axis_config[i].function)
				{
					case FUNCTION_PLUS_ABS:
						tmp[i] = tmp[i] + tmp[p_config->axis_config[i].source_secondary];
						break;
					case FUNCTION_PLUS_REL:
						tmp[i] = tmp[i] + tmp[p_config->axis_config[i].source_secondary] - AXIS_MIN_VALUE;
						break;
					case FUNCTION_MINUS_ABS:
						tmp[i] = tmp[i] - tmp[p_config->axis_config[i].source_secondary];
						break;
					case FUNCTION_MINUS_REL:
						tmp[i] = tmp[i] - tmp[p_config->axis_config[i].source_secondary] + AXIS_MIN_VALUE;
						break;
					default:
						break;
				}
			}
			if (tmp[i] > AXIS_MAX_VALUE) tmp[i] = AXIS_MAX_VALUE;
			else if (tmp[i] < AXIS_MIN_VALUE) tmp[i] = AXIS_MIN_VALUE;
		}
		
		
    // setting technical axis data
    scaled_axis_data[i] = tmp[i];
    // setting output axis data
    if (p_config->axis_config[i].out_enabled)  out_axis_data[i] = tmp[i];
    else  out_axis_data[i] = 0;
		
	}
	
}

/**
  * @brief  Resetting axis calibration values to the default
	*	@param	p_config: Pointer to device configuration structure
	*	@param	axis_num: Number of axis 
  * @retval None
  */
void AxisResetCalibration (app_config_t * p_config, uint8_t axis_num)
{
	p_config->axis_config[axis_num].calib_max = AXIS_MIN_VALUE;
	p_config->axis_config[axis_num].calib_center = AXIS_CENTER_VALUE;
	p_config->axis_config[axis_num].calib_min = AXIS_MAX_VALUE;
}

/**
  * @brief  Getting axes data in report format
	*	@param	out_data: Pointer to target buffer of axes output data (may be disabled in configuration)
	*	@param	scaled_data: Pointer to target buffer of axes scaled output data
	*	@param	raw_data: Pointer to target buffer of axes raw output data 
  * @retval None
  */
void AnalogGet (analog_data_t * out_data, analog_data_t * scaled_data, analog_data_t * raw_data)
{
	if (scaled_data != NULL)
	{
		memcpy(scaled_data, scaled_axis_data, sizeof(scaled_axis_data));
	}
	if (raw_data != NULL)
	{
		memcpy(raw_data, raw_axis_data, sizeof(raw_axis_data));
	}
	if (raw_data != NULL)
	{
		memcpy(out_data, out_axis_data, sizeof(raw_axis_data));
	}
}




