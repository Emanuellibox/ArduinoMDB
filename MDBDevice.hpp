#pragma once

#include "MDBSerial.hpp"
#include "SoftwareSerial.h"
#include <Arduino.h>

#define RESET 0x00
#define SETUP 0x01
#define POLL 0x03
#define TYPE 0x04
#define EXPANSION 0x07
#define IDENTIFICATION 0x00
#define FEATURE_ENABLE 0x01
#define PAYOUT 0x02

#define JUST_RESET 0x0B
#define NO_RESPONSE 2000

#define MAX_RESET 10

#define SETUP_TIME 200
#define RESPONSE_TIME 5

class MDBDevice
{
public:
	explicit
	MDBDevice(MDBSerial &mdb, void (*error)(String) = &Error, void (*warning)(String) = &Error) : m_mdb(&mdb), m_error(error), m_warning(warning) {}

	virtual bool Reset() = 0;

	virtual void Print() = 0;
	
	inline void SetSerial(SoftwareSerial &serial) { m_serial = &serial; }
	
private:
	static void Error(String t);

protected:
	virtual int poll() = 0;
	
	MDBSerial *m_mdb;
	SoftwareSerial *m_serial;
	
	void (*m_error)(String);
	void (*m_warning)(String);

	int m_resetCount;
	
	int m_count;
	char m_buffer[64];

	char m_feature_level;
	unsigned int m_country;

	unsigned long m_manufacturer_code;
	char m_serial_number[12];
	char m_model_number[12];
};