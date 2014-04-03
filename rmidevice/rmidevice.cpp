#include <stdio.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#include "rmidevice.h"

#define RMI_DEVICE_PDT_ENTRY_SIZE		6
#define RMI_DEVICE_PAGE_SELECT_REGISTER		0xFF
#define RMI_DEVICE_MAX_PAGE			0xFF
#define RMI_DEVICE_PAGE_SIZE			0x100
#define RMI_DEVICE_PAGE_SCAN_START		0x00e9
#define RMI_DEVICE_PAGE_SCAN_END		0x0005
#define RMI_DEVICE_F01_BASIC_QUERY_LEN		21
#define RMI_DEVICE_F01_PRODUCTINFO_MASK		0x7f
#define RMI_DEVICE_F01_QRY5_YEAR_MASK		0x1f
#define RMI_DEVICE_F01_QRY6_MONTH_MASK		0x0f
#define RMI_DEVICE_F01_QRY7_DAY_MASK		0x1f

#define RMI_DEVICE_F01_QRY1_HAS_LTS		(1 << 2)
#define RMI_DEVICE_F01_QRY1_HAS_SENSOR_ID	(1 << 3)
#define RMI_DEVICE_F01_QRY1_HAS_CHARGER_INP	(1 << 4)
#define RMI_DEVICE_F01_QRY1_HAS_ADJ_DOZE	(1 << 5)
#define RMI_DEVICE_F01_QRY1_HAS_ADJ_DOZE_HOFF	(1 << 6)
#define RMI_DEVICE_F01_QRY1_HAS_PROPS_2		(1 << 7)

#define RMI_DEVICE_F01_LTS_RESERVED_SIZE	19

#define RMI_DEVICE_F01_QRY42_DS4_QUERIES	(1 << 0)
#define RMI_DEVICE_F01_QRY42_MULTI_PHYS		(1 << 1)

#define RMI_DEVICE_F01_QRY43_01_PACKAGE_ID     (1 << 0)
#define RMI_DEVICE_F01_QRY43_01_BUILD_ID       (1 << 1)

#define PACKAGE_ID_BYTES			4
#define BUILD_ID_BYTES				3

#define RMI_F01_CMD_DEVICE_RESET	1
#define RMI_F01_DEFAULT_RESET_DELAY_MS	100

int RMIDevice::SetRMIPage(unsigned char page)
{
	return Write(RMI_DEVICE_PAGE_SELECT_REGISTER, &page, 1);
}

int RMIDevice::QueryBasicProperties()
{
	int rc;
	unsigned char basicQuery[RMI_DEVICE_F01_BASIC_QUERY_LEN];
	unsigned short queryAddr;
	unsigned char infoBuf[4];
	unsigned short prodInfoAddr;
	RMIFunction f01;

	if (GetFunction(f01, 1)) {
		queryAddr = f01.GetQueryBase();

		rc = Read(queryAddr, basicQuery, RMI_DEVICE_F01_BASIC_QUERY_LEN);
		if (rc < 0) {
			fprintf(stderr, "Failed to read the basic query: %s\n", strerror(errno));
			return rc;
		}
		m_manufacturerID = basicQuery[0];
		m_hasLTS = basicQuery[1] & RMI_DEVICE_F01_QRY1_HAS_LTS;
		m_hasSensorID = basicQuery[1] & RMI_DEVICE_F01_QRY1_HAS_SENSOR_ID;
		m_hasAdjustableDoze = basicQuery[1] & RMI_DEVICE_F01_QRY1_HAS_ADJ_DOZE;
		m_hasAdjustableDozeHoldoff = basicQuery[1] & RMI_DEVICE_F01_QRY1_HAS_ADJ_DOZE_HOFF;
		m_hasQuery42 = basicQuery[1] & RMI_DEVICE_F01_QRY1_HAS_PROPS_2;
		m_productInfo = ((basicQuery[2] & RMI_DEVICE_F01_PRODUCTINFO_MASK) << 7) |
				(basicQuery[3] & RMI_DEVICE_F01_PRODUCTINFO_MASK);

		snprintf(m_dom, sizeof(m_dom), "20%02d/%02d/%02d",
				basicQuery[5] & RMI_DEVICE_F01_QRY5_YEAR_MASK,
		 		basicQuery[6] & RMI_DEVICE_F01_QRY6_MONTH_MASK,
		 		basicQuery[7] & RMI_DEVICE_F01_QRY7_DAY_MASK);

		memcpy(m_productID, &basicQuery[11], RMI_PRODUCT_ID_LENGTH);
		m_productID[RMI_PRODUCT_ID_LENGTH] = '\0';

		queryAddr += 11;
		prodInfoAddr = queryAddr + 6;
		queryAddr += 10;

		if (m_hasLTS)
			++queryAddr;

		if (m_hasSensorID) {
			rc = Read(queryAddr++, &m_sensorID, 1);
			if (rc < 0) {
				fprintf(stderr, "Failed to read sensor id: %s\n", strerror(errno));
				return rc;
			}
		}

		if (m_hasLTS)
			queryAddr += RMI_DEVICE_F01_LTS_RESERVED_SIZE;

		if (m_hasQuery42) {
			rc = Read(queryAddr++, infoBuf, 1);
			if (rc < 0) {
				fprintf(stderr, "Failed to read query 42: %s\n", strerror(errno));
				return rc;
			}

			m_hasDS4Queries = infoBuf[0] & RMI_DEVICE_F01_QRY42_DS4_QUERIES;
			m_hasMultiPhysical = infoBuf[0] & RMI_DEVICE_F01_QRY42_MULTI_PHYS;
		}

		if (m_hasDS4Queries) {
			rc = Read(queryAddr++, &m_ds4QueryLength, 1);
			if (rc < 0) {
				fprintf(stderr, "Failed to read DS4 query length: %s\n", strerror(errno));
				return rc;
			}
		}

		for (int i = 1; i <= m_ds4QueryLength; ++i) {
			unsigned char val;
			rc = Read(queryAddr++, &val, 1);
			if (rc < 0) {
				fprintf(stderr, "Failed to read F01 Query43.%02d: %s\n", i, strerror(errno));
				continue;
			}

			switch(i) {
				case 1:
					m_hasPackageIDQuery = val & RMI_DEVICE_F01_QRY43_01_PACKAGE_ID;
					m_hasBuildIDQuery = val & RMI_DEVICE_F01_QRY43_01_BUILD_ID;
					break;
				case 2:
				case 3:
				default:
					break;
			}
		}

		if (m_hasPackageIDQuery) {
			rc = Read(prodInfoAddr++, infoBuf, PACKAGE_ID_BYTES);
			if (rc > 0) {
				unsigned short *val = (unsigned short *)infoBuf;
				m_packageID = *val;
				val = (unsigned short *)(infoBuf + 2);
				m_packageRev = *val;
			}
		}

		if (m_hasBuildIDQuery) {
			rc = Read(prodInfoAddr, infoBuf, BUILD_ID_BYTES);
			if (rc > 0) {
				unsigned short *val = (unsigned short *)infoBuf;
				m_buildID = *val;
				m_buildID += infoBuf[2] * 65536;
			}
		}
	}
	return 0;
}

void RMIDevice::PrintProperties()
{
	fprintf(stdout, "manufacturerID:\t\t%d\n", m_manufacturerID);
	fprintf(stdout, "Has LTS?:\t\t%d\n", m_hasLTS);
	fprintf(stdout, "Has Sensor ID?:\t\t%d\n", m_hasSensorID);
	fprintf(stdout, "Has Adjustable Doze?:\t%d\n", m_hasAdjustableDoze);
	fprintf(stdout, "Has Query 42?:\t\t%d\n", m_hasQuery42);
	fprintf(stdout, "Date of Manufacturer:\t%s\n", m_dom);
	fprintf(stdout, "Product ID:\t\t%s\n", m_productID);
	fprintf(stdout, "Product Info:\t\t%d\n", m_productInfo);
	fprintf(stdout, "Package ID:\t\t%d\n", m_packageID);
	fprintf(stdout, "Package Rev:\t\t%d\n", m_packageRev);
	fprintf(stdout, "Build ID:\t\t%ld\n", m_buildID);
	fprintf(stdout, "Sensor ID:\t\t%d\n", m_sensorID);
	fprintf(stdout, "Has DS4 Queries?:\t%d\n", m_hasDS4Queries);
	fprintf(stdout, "Has Multi Phys?:\t%d\n", m_hasMultiPhysical);
	fprintf(stdout, "\n");
}

int RMIDevice::Reset()
{
	int rc;
	RMIFunction f01;
	struct timespec ts;
	struct timespec rem;
	const unsigned char deviceReset = RMI_F01_CMD_DEVICE_RESET;

	if (!GetFunction(f01, 1))
		return -1;

	fprintf(stdout, "Resetting...\n");
	rc = Write(f01.GetCommandBase(), &deviceReset, 1);
	if (rc < 0)
		return rc;

	ts.tv_sec = RMI_F01_DEFAULT_RESET_DELAY_MS / 1000;
	ts.tv_nsec = (RMI_F01_DEFAULT_RESET_DELAY_MS % 1000) * 1000 * 1000;
	for (;;) {
		if (nanosleep(&ts, &rem) == 0) {
			break;
		} else {
			if (errno == EINTR) {
				ts = rem;
				continue;
			}
			return -1;
		}
	}
	fprintf(stdout, "Reset completed.\n");
	return 0;
}

bool RMIDevice::GetFunction(RMIFunction &func, int functionNumber)
{
	std::vector<RMIFunction>::iterator funcIter;

	for (funcIter = m_functionList.begin(); funcIter != m_functionList.end(); ++funcIter) {
		if (funcIter->GetFunctionNumber() == functionNumber) {
			func = *funcIter;
			return true;
		}
	}
	return false;
}

int RMIDevice::ScanPDT()
{
	int rc;
	unsigned int page;
	unsigned int addr;
	unsigned char entry[RMI_DEVICE_PDT_ENTRY_SIZE];

	m_functionList.clear();

	for (page = 0; page < RMI_DEVICE_MAX_PAGE; ++page) {
		unsigned int page_start = RMI_DEVICE_PAGE_SIZE * page;
		unsigned int pdt_start = page_start + RMI_DEVICE_PAGE_SCAN_START;
		unsigned int pdt_end = page_start + RMI_DEVICE_PAGE_SCAN_END;
		bool found = false;

		SetRMIPage(page);

		for (addr = pdt_start; addr >= pdt_end; addr -= RMI_DEVICE_PDT_ENTRY_SIZE) {
			rc = Read(addr, entry, RMI_DEVICE_PDT_ENTRY_SIZE);
			if (rc < 0) {
				fprintf(stderr, "Failed to read PDT entry at address (0x%04x)\n", addr);
				return rc;
			}
			
			RMIFunction func(entry);
			if (func.GetFunctionNumber() == 0)
				break;

			m_functionList.push_back(func);
			found = true;
		}

		if (!found)
			break;
	}

	return 0;
}