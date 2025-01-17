/****************************************************************************
 * arch/arm/src/samv7/sam_qspi_spi.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <debug.h>

#include <arch/board/board.h>

#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wdog.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "arm_internal.h"
#include "sam_gpio.h"
#include "sam_xdmac.h"
#include "sam_periphclks.h"
#include "sam_qspi_spi.h"
#include "hardware/sam_pmc.h"
#include "hardware/sam_xdmac.h"
#include "hardware/sam_qspi.h"
#include "hardware/sam_pinmap.h"

#ifdef CONFIG_SAMV7_QSPI_SPI_MODE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking *****************************************************************/

/* The SPI Baud rate clock is generated by dividing the peripheral clock by
 * a value between 1 and 255
 */

#define SAM_QSPI_CLOCK     BOARD_MCK_FREQUENCY  /* Frequency of the main clock */

struct sam_spics_s
{
  struct spi_dev_s spidev;     /* Externally visible part of the SPI interface */
  uint32_t frequency;          /* Requested clock frequency */
  uint32_t actual;             /* Actual clock frequency */
  uint8_t mode;                /* Mode 0,1,2,3 */
  uint8_t nbits;               /* Width of word in bits (8 to 16) */
};

/* Type of board-specific SPI status function */

typedef void (*select_t)(uint32_t devid, bool selected);

/* The overall state of one SPI controller */

struct sam_spidev_s
{
  uint32_t base;               /* SPI controller register base address */
  mutex_t spilock;             /* Assures mutually exclusive access to SPI */
  select_t select;             /* SPI select call-out */
  bool initialized;            /* TRUE: Controller has been initialized */
  bool escape_lastxfer;        /* Don't set LASTXFER-Bit in the next transfer */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t qspi_getreg(struct sam_spidev_s *spi,
                                   unsigned int offset);
static inline void qspi_putreg(struct sam_spidev_s *spi, uint32_t value,
                               unsigned int offset);

static inline void qspi_flush(struct sam_spidev_s *spi);

/* SPI master methods */

static int  qspi_spi_lock(struct spi_dev_s *dev, bool lock);
static void qspi_spi_select(struct spi_dev_s *dev, uint32_t devid,
                            bool selected);
static uint32_t qspi_spi_setfrequency(struct spi_dev_s *dev,
                                      uint32_t frequency);
static void qspi_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode);
static void qspi_spi_setbits(struct spi_dev_s *dev, int nbits);
static uint32_t qspi_spi_send(struct spi_dev_s *dev, uint32_t wd);
static void qspi_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                              void *rxbuffer, size_t nwords);
#ifndef CONFIG_SPI_EXCHANGE
static void     qspi_spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                                  size_t nwords);
static void     qspi_spi_recvblock(struct spi_dev_s *dev, void *buffer,
                                   size_t nwords);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* QSPI_SPI driver operations */

static const struct spi_ops_s g_spiops =
{
  .lock              = qspi_spi_lock,
  .select            = qspi_spi_select,
  .setfrequency      = qspi_spi_setfrequency,
  .setmode           = qspi_spi_setmode,
  .setbits           = qspi_spi_setbits,
  .status            = sam_qspi_status,
#ifdef CONFIG_SPI_CMDDATA
  .cmddata           = sam_qspi_cmddata,
#endif
  .send              = qspi_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange          = qspi_spi_exchange,
#else
  .sndblock          = qspi_spi_sndblock,
  .recvblock         = qspi_spi_recvblock,
#endif
  .registercallback  = 0,                 /* Not implemented */
};

/* This is the overall state of the SPI0 controller */

static struct sam_spidev_s g_spidev =
{
  .base              = SAM_QSPI_BASE,
  .spilock           = NXMUTEX_INITIALIZER,
  .select            = sam_qspi_select,
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qspi_spi_getreg
 *
 * Description:
 *  Read a QSPI register
 *
 ****************************************************************************/

static inline uint32_t qspi_getreg(struct sam_spidev_s *spi,
                                  unsigned int offset)
{
  uint32_t address = spi->base + offset;
  uint32_t value = getreg32(address);

  return value;
}

/****************************************************************************
 * Name: qspi_putreg
 *
 * Description:
 *  Write a value to a QSPI register
 *
 ****************************************************************************/

static inline void qspi_putreg(struct sam_spidev_s *spi, uint32_t value,
                              unsigned int offset)
{
  uint32_t address = spi->base + offset;

  putreg32(value, address);
}

/****************************************************************************
 * Name: qspi_flush
 *
 * Description:
 *   Make sure that there are now dangling SPI transfer in progress
 *
 * Input Parameters:
 *   spi - SPI controller state
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void qspi_flush(struct sam_spidev_s *spi)
{
  /* Make sure the no TX activity is in progress... waiting if necessary */

  while ((qspi_getreg(spi, SAM_QSPI_SR_OFFSET) & QSPI_INT_TXEMPTY) == 0);

  /* Then make sure that there is no pending RX data .. reading as
   * discarding as necessary.
   */

  while ((qspi_getreg(spi, SAM_QSPI_SR_OFFSET) & QSPI_INT_RDRF) != 0)
    {
       qspi_getreg(spi, SAM_QSPI_RDR_OFFSET);
    }
}

/****************************************************************************
 * Name: qspi_spi_lock
 *
 * Description:
 *   On SPI buses where there are multiple devices, it will be necessary to
 *   lock SPI to have exclusive access to the buses for a sequence of
 *   transfers.  The bus should be locked before the chip is selected. After
 *   locking the SPI bus, the caller should then also call the setfrequency,
 *   setbits, and setmode methods to make sure that the SPI is properly
 *   configured for the device.  If the SPI bus is being shared, then it
 *   may have been left in an incompatible state.
 *
 * Input Parameters:
 *   dev  - Device-specific state data
 *   lock - true: Lock spi bus, false: unlock SPI bus
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static int qspi_spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct sam_spidev_s *spi = &g_spidev;
  int ret;

  spiinfo("lock=%d\n", lock);
  if (lock)
    {
      ret = nxmutex_lock(&spi->spilock);
    }
  else
    {
      ret = nxmutex_unlock(&spi->spilock);
    }

  return ret;
}

/****************************************************************************
 * Name: qspi_spi_select
 *
 * Description:
 *   This function does not actually set the chip select line.  Rather, it
 *   simply maps the device ID into a chip select number and retains that
 *   chip select number for later use.
 *
 * Input Parameters:
 *   dev -       Device-specific state data
 *   frequency - The SPI frequency requested
 *
 * Returned Value:
 *   Returns the actual frequency selected
 *
 ****************************************************************************/

static void qspi_spi_select(struct spi_dev_s *dev, uint32_t devid,
                            bool selected)
{
  struct sam_spidev_s *spi = &g_spidev;

  /* QSPI has just one CS so there is no need to perform any operation */

  spi->select(devid, selected);
}

/****************************************************************************
 * Name: qspi_spi_setfrequency
 *
 * Description:
 *   Set the QSPI frequency.
 *
 * Input Parameters:
 *   dev -       Device-specific state data
 *   frequency - The QSPI frequency requested
 *
 * Returned Value:
 *   Returns the actual frequency selected
 *
 ****************************************************************************/

static uint32_t qspi_spi_setfrequency(struct spi_dev_s *dev,
                                      uint32_t frequency)
{
  struct sam_spics_s *spics = (struct sam_spics_s *)dev;
  struct sam_spidev_s *spi = &g_spidev;
  uint32_t actual;
  uint32_t scbr;
  uint32_t dlybct;
  uint32_t regval;

  spiinfo("frequency=%ld\n", frequency);

  /* Check if the requested frequency is the same as the frequency
   * selection
   */

  if (spics->frequency == frequency)
    {
      /* We are already at this frequency.  Return the actual. */

      return spics->actual;
    }

  /* Configure QSPI to a frequency as close as possible to the requested
   * frequency.
   *
   *   QSCK frequency = QSPI_CLK / SCBR, or SCBR = QSPI_CLK / frequency
   *
   * Where SCBR can have the range 1 to 256 and the SCR register field holds
   * SCBR - 1.  NOTE that a "ceiling" type of calculation is performed.
   * 'frequency' is treated as a not-to-exceed value.
   */

  scbr = (frequency + SAM_QSPI_CLOCK - 1) / frequency;

  /* Make sure that the divider is within range */

  if (scbr < 1)
    {
      scbr = 1;
    }
  else if (scbr > 256)
    {
      scbr = 256;
    }

  /* Save the new SCBR value (minus one) */

  regval  = qspi_getreg(spi, SAM_QSPI_SCR_OFFSET);
  regval &= ~(QSPI_SCR_SCBR_MASK | QSPI_SCR_DLYBS_MASK);
  regval |= (scbr - 1) << QSPI_SCR_SCBR_SHIFT;

  qspi_putreg(spi, regval, SAM_QSPI_SCR_OFFSET);

  /* DLYBCT: Delay Between Consecutive Transfers.  This field defines the
   * delay between two consecutive transfers with the same peripheral without
   * removing the chip select. The delay is always inserted after each
   * transfer and before removing the chip select if needed.
   *
   *  Delay Between Consecutive Transfers = (32 x DLYBCT) / SPI_CLK
   *
   * For a 5uS delay:
   *
   *  DLYBCT = SPI_CLK * 0.000005 / 32 = SPI_CLK / 200000 / 32
   */

  dlybct  = SAM_QSPI_CLOCK / 200000 / 32;
  regval  = qspi_getreg(spi, SAM_QSPI_MR_OFFSET);
  regval &= ~(QSPI_MR_DLYBCT_MASK);
  regval |= dlybct << QSPI_MR_DLYBCT_SHIFT;
  qspi_putreg(spi, regval, SAM_QSPI_MR_OFFSET);

  /* Calculate the new actual frequency */

  actual = SAM_QSPI_CLOCK / scbr;
  spiinfo("SCBR=%ld actual=%ld\n", scbr, actual);

  /* Save the frequency setting */

  spics->frequency = frequency;
  spics->actual    = actual;

  spiinfo("Frequency %ld->%ld\n", frequency, actual);
  return actual;
}

/****************************************************************************
 * Name: qspi_spi_setmode
 *
 * Description:
 *   Set the SPI mode. Optional. See enum spi_mode_e for mode definitions
 *
 * Input Parameters:
 *   dev -  Device-specific state data
 *   mode - The SPI mode requested
 *
 * Returned Value:
 *   none
 *
 ****************************************************************************/

static void qspi_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct sam_spics_s *spics = (struct sam_spics_s *)dev;
  struct sam_spidev_s *spi = &g_spidev;
  uint32_t regval;

  spiinfo("mode=%d\n", mode);

  /* Has the mode changed? */

  if (mode != spics->mode)
    {
      /* Yes... Set the mode appropriately:
       *
       * MODE
       * SPI  CPOL NCPHA
       *  0    0    1
       *  1    0    0
       *  2    1    1
       *  3    1    0
       */

      regval  = qspi_getreg(spi, SAM_QSPI_SCR_OFFSET);
      regval &= ~(QSPI_SCR_CPOL | QSPI_SCR_CPHA);

      switch (mode)
        {
        case SPIDEV_MODE0: /* CPOL=0; CPHA=0 */
          break;

        case SPIDEV_MODE1: /* CPOL=0; CPHA=1 */
          regval |= QSPI_SCR_CPHA;
          break;

        case SPIDEV_MODE2: /* CPOL=1; CPHA=0 */
          regval |= QSPI_SCR_CPOL;
          break;

        case SPIDEV_MODE3: /* CPOL=1; CPHA=1 */
          regval |= (QSPI_SCR_CPOL | QSPI_SCR_CPHA);
          break;

        default:
          DEBUGASSERT(FALSE);
          return;
        }

      qspi_putreg(spi, regval, SAM_QSPI_SCR_OFFSET);

      /* Save the mode so that subsequent re-configurations will be faster */

      spics->mode = mode;
    }
}

/****************************************************************************
 * Name: qspi_spi_setbits
 *
 * Description:
 *   Set the number if bits per word.
 *
 * Input Parameters:
 *   dev -  Device-specific state data
 *   nbits - The number of bits requested
 *
 * Returned Value:
 *   none
 *
 ****************************************************************************/

static void qspi_spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct sam_spics_s *spics = (struct sam_spics_s *)dev;
  struct sam_spidev_s *spi = &g_spidev;
  uint32_t regval;

  spiinfo("nbits=%d\n", nbits);
  DEBUGASSERT(nbits > 7 && nbits < 17);

  /* Has the number of bits changed? */

  if (nbits != spics->nbits)
    {
      /* Yes... Set number of bits appropriately */

      regval  = qspi_getreg(spi, SAM_QSPI_MR_OFFSET);
      regval &= ~QSPI_MR_NBBITS_MASK;
      regval |= QSPI_MR_NBBITS(nbits);
      qspi_putreg(spi, regval, SAM_QSPI_MR_OFFSET);

      /* Save the selection so that subsequent re-configurations will be
       * faster.
       */

      spics->nbits = nbits;
    }
}

/****************************************************************************
 * Name: qspi_spi_send
 *
 * Description:
 *   Exchange one word on SPI
 *
 * Input Parameters:
 *   dev - Device-specific state data
 *   wd  - The word to send.  the size of the data is determined by the
 *         number of bits selected for the SPI interface.
 *
 * Returned Value:
 *   response
 *
 ****************************************************************************/

static uint32_t qspi_spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  struct sam_spics_s *spics = (struct sam_spics_s *)dev;
  if (spics->nbits <= 8)
    {
      uint8_t txbyte;
      uint8_t rxbyte;

      txbyte = (uint8_t)wd;
      rxbyte = (uint8_t)0;
      qspi_spi_exchange(dev, &txbyte, &rxbyte, 1);

      spiinfo("Sent %02x received %02x\n", txbyte, rxbyte);
      return (uint32_t)rxbyte;
    }
  else
    {
      uint16_t txword;
      uint16_t rxword;

      txword = (uint16_t)wd;
      rxword = (uint16_t)0;
      qspi_spi_exchange(dev, &txword, &rxword, 1);

      spiinfo("Sent %02x received %02x\n", txword, rxword);
      return (uint32_t)rxword;
    }
}

/****************************************************************************
 * Name: qspi_spi_exchange
 *
 * Description:
 *   Exchange a block of data from SPI.
 *
 * Input Parameters:
 *   dev      - Device-specific state data
 *   txbuffer - A pointer to the buffer of data to be sent
 *   rxbuffer - A pointer to the buffer in which to receive data
 *   nwords   - the length of data that to be exchanged in units of words.
 *              The wordsize is determined by the number of bits-per-word
 *              selected for the SPI interface.  If nbits <= 8, the data is
 *              packed into uint8_t's; if nbits >8, the data is packed into
 *              uint16_t's
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void qspi_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                              void *rxbuffer, size_t nwords)
{
  struct sam_spics_s *spics = (struct sam_spics_s *)dev;
  struct sam_spidev_s *spi = &g_spidev;
  uint32_t data;
  uint16_t *rxptr16;
  uint16_t *txptr16;
  uint8_t *rxptr8;
  uint8_t *txptr8;

  spiinfo("txbuffer=%p rxbuffer=%p nwords=%d\n", txbuffer, rxbuffer, nwords);

  /* Set up working pointers */

  if (spics->nbits > 8)
    {
      rxptr16 = (uint16_t *)rxbuffer;
      txptr16 = (uint16_t *)txbuffer;
      rxptr8  = NULL;
      txptr8  = NULL;
    }
  else
    {
      rxptr16 = NULL;
      txptr16 = NULL;
      rxptr8  = (uint8_t *)rxbuffer;
      txptr8  = (uint8_t *)txbuffer;
    }

  /* Make sure that any previous transfer is flushed from the hardware */

  qspi_flush(spi);

  /* Loop, sending each word in the user-provided data buffer.
   *
   * Note 1: Good SPI performance would require that we implement DMA
   *         transfers!
   * Note 2: This loop might be made more efficient.  Would logic
   *         like the following improve the throughput?  Or would it
   *         just add the risk of overruns?
   *
   *   Get word 1;
   *   Send word 1;  Now word 1 is "in flight"
   *   nwords--;
   *   for ( ; nwords > 0; nwords--)
   *     {
   *       Get word N.
   *       Wait for TDRE meaning that word N-1 has moved to the shift
   *          register.
   *       Disable interrupts to keep the following atomic
   *       Send word N.  Now both work N-1 and N are "in flight"
   *       Wait for RDRF meaning that word N-1 is available
   *       Read word N-1.
   *       Re-enable interrupts.
   *       Save word N-1.
   *     }
   *   Wait for RDRF meaning that the final word is available
   *   Read the final word.
   *   Save the final word.
   */

  for (; nwords > 0; nwords--)
    {
      /* Get the data to send (0xff if there is no data source). */

      if (txptr8)
        {
          data = (uint32_t)*txptr8++;
        }
      else if (txptr16)
        {
          data = (uint32_t)*txptr16++;
        }
      else
        {
          data = 0xffff;
        }

      /* Wait for any previous data written to the TDR to be transferred
       * to the serializer.
       */

      while ((qspi_getreg(spi, SAM_QSPI_SR_OFFSET) & QSPI_INT_TDRE) == 0);

      /* Write the data to transmitted to the Transmit Data Register (TDR) */

      qspi_putreg(spi, data, SAM_QSPI_TDR_OFFSET);

      /* Wait for the read data to be available in the RDR.
       * TODO:  Data transfer rates would be improved using the RX FIFO
       *        (and also DMA)
       */

      while ((qspi_getreg(spi, SAM_QSPI_SR_OFFSET) & QSPI_INT_RDRF) == 0);

      /* Read the received data from the SPI Data Register. */

      data = qspi_getreg(spi, SAM_QSPI_RDR_OFFSET);
      if (rxptr8)
        {
          *rxptr8++ = (uint8_t)data;
        }
      else if (rxptr16)
        {
          *rxptr16++ = (uint16_t)data;
        }
    }
}

/****************************************************************************
 * Name: qspi_spi_sndblock
 *
 * Description:
 *   Send a block of data on SPI
 *
 * Input Parameters:
 *   dev -    Device-specific state data
 *   buffer - A pointer to the buffer of data to be sent
 *   nwords - the length of data to send from the buffer in number of words.
 *            The wordsize is determined by the number of bits-per-word
 *            selected for the SPI interface.  If nbits <= 8, the data is
 *            packed into uint8_t's; if nbits >8, the data is packed into
 *            uint16_t's
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifndef CONFIG_SPI_EXCHANGE
static void qspi_spi_sndblock(struct spi_dev_s *dev, const void *buffer,
                         size_t nwords)
{
  /* spi_exchange can do this. */

  qspi_spi_exchange(dev, buffer, NULL, nwords);
}

/****************************************************************************
 * Name: qspi_spi_recvblock
 *
 * Description:
 *   Revice a block of data from SPI
 *
 * Input Parameters:
 *   dev -    Device-specific state data
 *   buffer - A pointer to the buffer in which to receive data
 *   nwords - the length of data that can be received in the buffer in number
 *            of words.  The wordsize is determined by the number of
 *            bits-per-word selected for the SPI interface.  If nbits <= 8,
 *            the data is packed into uint8_t's; if nbits >8, the data is
 *            packed into uint16_t's
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void qspi_spi_recvblock(struct spi_dev_s *dev, void *buffer,
                               size_t nwords)
{
  /* spi_exchange can do this. */

  qspi_spi_exchange(dev, NULL, buffer, nwords);
}
#endif  /* CONFIG_SPI_EXCHANGE */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sam_qspi_spi_initialize
 *
 * Description:
 *   Initialize the selected SPI port in master mode
 *
 * Input Parameters:
 *   cs - Chip select number (identifying the "logical" SPI port)
 *
 * Returned Value:
 *   Valid SPI device structure reference on success; a NULL on failure
 *
 ****************************************************************************/

struct spi_dev_s *sam_qspi_spi_initialize(int intf)
{
  struct sam_spidev_s *spi;
  struct sam_spics_s *spics;
  irqstate_t flags;
  uint32_t regval;

  /* The supported SAM parts have only a single QSPI port */

  spiinfo("intf: %d\n", intf);
  DEBUGASSERT(intf >= 0 && intf < (SAMV7_NQSPI_SPI + SAMV7_NQSPI));

  /* Allocate a new state structure for this chip select. */

  spics = (struct sam_spics_s *)kmm_zalloc(sizeof(struct sam_spics_s));
  if (!spics)
    {
      spierr("ERROR: Failed to allocate a chip select structure\n");
      return NULL;
    }

  /* Select the SPI operations */

  spics->spidev.ops = &g_spiops;

  /* Get the SPI device structure associated with the chip select */

  spi = &g_spidev;

  /* Has the SPI hardware been initialized? */

  if (!spi->initialized)
    {
      flags = enter_critical_section();
      sam_qspi_enableclk();

      /* Configure multiplexed pins as connected on the board.  Chip
       * select pins must be selected by board-specific logic.
       */

      sam_configgpio(GPIO_QSPI_IO0);    /* MOSI */
      sam_configgpio(GPIO_QSPI_IO1);    /* MISO */
      sam_configgpio(GPIO_QSPI_SCK);

      /* Disable write protection */

      qspi_putreg(spi, SAM_QSPI_WPCR_OFFSET, QSPI_WPCR_WPKEY);

      /* Disable QSPI before configuring it */

      qspi_putreg(spi, QSPI_CR_QSPIDIS, SAM_QSPI_CR_OFFSET);

      /* Execute a software reset of the QSPI (twice) */

      qspi_putreg(spi, QSPI_CR_SWRST, SAM_QSPI_CR_OFFSET);
      qspi_putreg(spi, QSPI_CR_SWRST, SAM_QSPI_CR_OFFSET);
      leave_critical_section(flags);

      /* Configure the QSPI mode register - select SPI mode */

      qspi_putreg(spi, QSPI_MR_SPI, SAM_QSPI_MR_OFFSET);

      /* And enable the SPI */

      qspi_putreg(spi, QSPI_CR_QSPIEN, SAM_QSPI_CR_OFFSET);
      up_mdelay(20);

      /* Flush any pending transfers */

      qspi_getreg(spi, SAM_QSPI_SR_OFFSET);
      qspi_getreg(spi, SAM_QSPI_RDR_OFFSET);

      spi->initialized = true;
    }

  /* Set to mode=0 and nbits=8 and impossible frequency. The SPI will only
   * be reconfigured if there is a change.
   */

  regval = qspi_getreg(spi, SAM_QSPI_SCR_OFFSET);
  regval &= ~(QSPI_SCR_CPOL | QSPI_SCR_CPHA);
  qspi_putreg(spi, regval, SAM_QSPI_SCR_OFFSET);
  spics->mode = 0;

  regval = qspi_getreg(spi, SAM_QSPI_MR_OFFSET);
  regval |= QSPI_MR_NBBITS_8BIT;
  qspi_putreg(spi, regval, SAM_QSPI_MR_OFFSET);
  spics->nbits = 8;

  spics->frequency = 0;

  return &spics->spidev;
}
#endif /* CONFIG_SAMV7_QSPI_SPI_MODE */
