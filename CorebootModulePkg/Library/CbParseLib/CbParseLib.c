/** @file
  This library will parse the coreboot table in memory and extract those required
  information.

  Copyright (c) 2014, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <Uefi/UefiBaseType.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/CbParseLib.h>

#include <IndustryStandard/Acpi.h>

#include "Coreboot.h"

/* Helpful inlines */

static UINT64 cb_unpack64(struct cbuint64 val)
{
  return LShiftU64 (val.hi, 32) | val.lo;
}

UINT16
CbCheckSum16 (
  IN UINT16   *Buffer,
  IN UINTN    Length
  )
{
	UINT32 Sum, TmpValue;
	UINTN  Idx;
	UINT8  *TmpPtr;
	
	Sum = 0;
	TmpPtr = (UINT8 *)Buffer;
	for(Idx = 0; Idx < Length; Idx++) {
		TmpValue  = TmpPtr[Idx];
		if (Idx % 2 == 1) {
			TmpValue <<= 8;
		}
		
		Sum += TmpValue;
		
		// Wrap
		if (Sum >= 0x10000) {
			Sum = (Sum + (Sum >> 16)) & 0xFFFF;
		}
	}
	
	return (UINT16)((~Sum) & 0xFFFF);		
}

VOID *
FindCbTag (
  IN  VOID    *Start,
  IN  UINT32   Tag
  )
{
  struct cb_header   *Header;
  struct cb_record   *Record;
  UINT8              *TmpPtr;  
  UINT8              *TagPtr;  
  UINTN              Idx;  
  UINT16             CheckSum;
      
  Header = NULL;
  TmpPtr = (UINT8 *)Start;
  for (Idx = 0; Idx < 4096; Idx += 16, TmpPtr += 16) {
    Header = (struct cb_header   *)TmpPtr;
    if (Header->signature == CB_HEADER_SIGNATURE) {
      break;
    }
  }
  
  if (Idx >= 4096)
  	return NULL;
  
  if (Header == NULL || !Header->table_bytes)
    return NULL;
    
  //
  // Check the checksum of the coreboot table header
  //
  CheckSum = CbCheckSum16 ((UINT16 *)Header, sizeof (*Header));
  if (CheckSum != 0) {
  	DEBUG ((EFI_D_ERROR, "Invalid coreboot table header checksum\n"));
  	return NULL;
  }  
  
  CheckSum = CbCheckSum16 ((UINT16 *)(TmpPtr + sizeof (*Header)), Header->table_bytes);
  if (CheckSum != Header->table_checksum) {
  	DEBUG ((EFI_D_ERROR, "Incorrect checksum of all the coreboot table entries\n"));
  	return NULL;
  }
     
  TagPtr = NULL;
  TmpPtr += Header->header_bytes;
  for (Idx = 0; Idx < Header->table_entries; Idx++) {
    Record = (struct cb_record *)TmpPtr;        
    if (Record->tag == CB_TAG_FORWARD) {
      TmpPtr = (VOID *)(UINTN)((struct cb_forward *)(UINTN)Record)->forward;
      if (Tag == CB_TAG_FORWARD)
        return TmpPtr;
      else 
        return FindCbTag (TmpPtr, Tag);
    }       
    if (Record->tag == Tag) {
      TagPtr = TmpPtr;
      break;
    }
    TmpPtr += Record->size;    
  }
    
  return TagPtr;
}

RETURN_STATUS
FindCbMemTable (  
  struct  cbmem_root  *root,
  IN UINT32     TableId, 
  IN VOID**     pMemTable,
  IN UINT32*    pMemTableSize
)
{	
	UINTN Idx;
	
	if ((!root) || (!pMemTable))
		return RETURN_INVALID_PARAMETER;
		
	for (Idx = 0; Idx < root->num_entries; Idx++) {
    if (root->entries[Idx].id == TableId) {
    	*pMemTable = (VOID *) (UINTN)root->entries[Idx].start;
    	if (pMemTableSize)
    		*pMemTableSize = root->entries[Idx].size;
    	
    	DEBUG ((EFI_D_ERROR, "Find CbMemTable Id 0x%x, base %p, size 0x%x\n", TableId, *pMemTable, *pMemTableSize));
    	return RETURN_SUCCESS;
    }
  }
  
  return RETURN_NOT_FOUND;	
}


/**
  Acquire the memory information from the coreboot table in memory.

  @param  pLowMemorySize     Pointer to the variable of low memory size
  @param  pHighMemorySize    Pointer to the variable of high memory size

  @retval RETURN_SUCCESS     Successfully find out the memory information.
  @retval RETURN_INVALID_PARAMETER    Invalid input parameters.
  @retval RETURN_NOT_FOUND   Failed to find the memory information.

**/
RETURN_STATUS
CbParseMemoryInfo (
  IN UINT64*    pLowMemorySize,
  IN UINT64*    pHighMemorySize
  )
{
	struct cb_memory*        rec;
	struct cb_memory_range*  Range;
  UINT64                   Start;
  UINT64                   Size;
	UINTN                    Index;
	
	if ((!pLowMemorySize) || (!pHighMemorySize))
		return RETURN_INVALID_PARAMETER;
	
	//
	// Get the coreboot memory table
	//
	rec = (struct cb_memory *)FindCbTag (0, CB_TAG_MEMORY);
	if (!rec) 
	  rec = (struct cb_memory *)FindCbTag ((VOID *)(UINTN)PcdGet32 (PcdCbHeaderPointer), CB_TAG_MEMORY);
		
	if (!rec) 
		return RETURN_NOT_FOUND;
		
	*pLowMemorySize = 0;
	*pHighMemorySize = 0;
		
	for (Index = 0; Index < MEM_RANGE_COUNT(rec); Index++) {
    Range = MEM_RANGE_PTR(rec, Index);    
    Start = cb_unpack64(Range->start);
    Size = cb_unpack64(Range->size);
    DEBUG ((EFI_D_ERROR, "%d. %016lx - %016lx [%02x]\n",
      Index, Start, Start + Size - 1, Range->type));
    
    if (Range->type != CB_MEM_RAM) {
      continue;
    }
      
    if (Start + Size < 0x100000000ULL) {
      *pLowMemorySize = Start + Size;
    } else {      
      *pHighMemorySize = Start + Size - 0x100000000ULL;
    }
  }
  
  DEBUG ((EFI_D_ERROR, "Low memory 0x%lx, High Memory 0x%lx\n", *pLowMemorySize, *pHighMemorySize));
  
  return RETURN_SUCCESS;	
}


/**
  Acquire the coreboot memory table with the given table id

  @param  TableId            Table id to be searched
  @param  pMemTable          Pointer to the base address of the memory table
  @param  pMemTableSize      Pointer to the size of the memory table

  @retval RETURN_SUCCESS     Successfully find out the memory table.
  @retval RETURN_INVALID_PARAMETER  Invalid input parameters.
  @retval RETURN_NOT_FOUND   Failed to find the memory table.

**/
RETURN_STATUS
CbParseCbMemTable (
  IN UINT32     TableId, 
  IN VOID**     pMemTable,
  IN UINT32*    pMemTableSize
  )
{
	struct cb_memory*        rec;
	struct cb_memory_range*  Range;
  UINT64                   Start;
  UINT64                   Size;
	UINTN                    Index;
	
	if (!pMemTable)
		return RETURN_INVALID_PARAMETER;
		
	*pMemTable = NULL;
	
	//
	// Get the coreboot memory table
	//
	rec = (struct cb_memory *)FindCbTag (0, CB_TAG_MEMORY);
	if (!rec)
		rec = (struct cb_memory *)FindCbTag ((VOID *)(UINTN)PcdGet32 (PcdCbHeaderPointer), CB_TAG_MEMORY);
		
	if (!rec)
		return RETURN_NOT_FOUND;
		
	for (Index = 0; Index < MEM_RANGE_COUNT(rec); Index++) {
    Range = MEM_RANGE_PTR(rec, Index);    
    Start = cb_unpack64(Range->start);
    Size = cb_unpack64(Range->size);
    
    if ((Range->type == CB_MEM_TABLE) && (Start > 0x1000)) {
        if (FindCbMemTable ((struct  cbmem_root *)(UINTN)(Start + Size - DYN_CBMEM_ALIGN_SIZE), TableId, pMemTable, pMemTableSize) == RETURN_SUCCESS)
        	return RETURN_SUCCESS;
    }
  }
	
	return RETURN_NOT_FOUND;
}


/**
  Acquire the acpi table from coreboot

  @param  pMemTable          Pointer to the base address of the memory table
  @param  pMemTableSize      Pointer to the size of the memory table

  @retval RETURN_SUCCESS     Successfully find out the memory table.
  @retval RETURN_INVALID_PARAMETER  Invalid input parameters.
  @retval RETURN_NOT_FOUND   Failed to find the memory table.

**/
RETURN_STATUS
CbParseAcpiTable (
  IN VOID*      pMemTable,
  IN UINT32*    pMemTableSize
  )
{
	return CbParseCbMemTable (SIGNATURE_32 ('I', 'P', 'C', 'A'), (VOID **)pMemTable, pMemTableSize);	
}

/**
  Acquire the smbios table from coreboot

  @param  pMemTable          Pointer to the base address of the memory table
  @param  pMemTableSize      Pointer to the size of the memory table

  @retval RETURN_SUCCESS     Successfully find out the memory table.
  @retval RETURN_INVALID_PARAMETER  Invalid input parameters.
  @retval RETURN_NOT_FOUND   Failed to find the memory table.

**/
RETURN_STATUS
CbParseSmbiosTable (
  IN VOID**     pMemTable,
  IN UINT32*    pMemTableSize
  )
{
	return CbParseCbMemTable (SIGNATURE_32 ('T', 'B', 'M', 'S'), pMemTable, pMemTableSize);	
}

/**
  Find the required fadt information

  @param  pPmCtrlReg         Pointer to the address of power management control register
  @param  pPmTimerReg        Pointer to the address of power management timer register
  @param  pResetReg          Pointer to the address of system reset register
  @param  pResetValue        Pointer to the value to be writen to the system reset register

  @retval RETURN_SUCCESS     Successfully find out all the required fadt information.
  @retval RETURN_NOT_FOUND   Failed to find the fadt table.

**/
RETURN_STATUS
CbParseFadtInfo (
  IN UINTN*     pPmCtrlReg,
  IN UINTN*     pPmTimerReg,
  IN UINTN*     pResetReg,
  IN UINTN*     pResetValue
  )
{
	EFI_ACPI_3_0_ROOT_SYSTEM_DESCRIPTION_POINTER* Rsdp;
	EFI_ACPI_DESCRIPTION_HEADER*                  Rsdt;
  UINT32*                                       Entry32;
  UINTN                                         Entry32Num;
  EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE*    Fadt;
  EFI_ACPI_DESCRIPTION_HEADER*                  Xsdt; 
  UINT64*                                       Entry64;
  UINTN                                         Entry64Num;
	UINTN                                         Idx;
	RETURN_STATUS                                 Status;
	
	Rsdp = NULL;
	Status = RETURN_SUCCESS;
	
	Status = CbParseAcpiTable (&Rsdp, NULL);
	if (RETURN_ERROR(Status))
		return Status;
		
	if (!Rsdp)
		return RETURN_NOT_FOUND;
		
	DEBUG ((EFI_D_ERROR, "Find Rsdp at %p\n", Rsdp));
	DEBUG ((EFI_D_ERROR, "Find Rsdt 0x%x, Xsdt 0x%lx\n", Rsdp->RsdtAddress, Rsdp->XsdtAddress));
	
	//
	// Search Rsdt First
	//
	Rsdt     = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Rsdp->RsdtAddress);  
	if (Rsdt != NULL) {
	  Entry32  = (UINT32 *)(Rsdt + 1);
	  Entry32Num = (Rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) >> 2;
	  for (Idx = 0; Idx < Entry32Num; Idx++) {
	    if (*(UINT32 *)(UINTN)(Entry32[Idx]) == EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
	      Fadt = (EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE *)(UINTN)(Entry32[Idx]);  
	      if (pPmCtrlReg)
	      	*pPmCtrlReg = Fadt->Pm1aCntBlk;	      	
	      DEBUG ((EFI_D_ERROR, "PmCtrl Reg 0x%x\n", Fadt->Pm1aCntBlk));
	      	
	      if (pPmTimerReg)   
	      	*pPmTimerReg = Fadt->PmTmrBlk; 
	      DEBUG ((EFI_D_ERROR, "PmTimer Reg 0x%x\n", Fadt->PmTmrBlk));
	      	
	      if (pResetReg)   
	      	*pResetReg = (UINTN)Fadt->ResetReg.Address; 
	      DEBUG ((EFI_D_ERROR, "Reset Reg 0x%lx\n", Fadt->ResetReg.Address));
	      	
	      if (pResetValue)   
	      	*pResetValue = Fadt->ResetValue;
	      DEBUG ((EFI_D_ERROR, "Reset Value 0x%x\n", Fadt->ResetValue));
	      	      	         
	      return RETURN_SUCCESS;        
	    }
	  }
	}
	
	//
	// Search Xsdt Second
	//
	Xsdt     = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(Rsdp->XsdtAddress);  
	if (Xsdt != NULL) {
	  Entry64  = (UINT64 *)(Xsdt + 1);
	  Entry64Num = (Xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) >> 3;
	  for (Idx = 0; Idx < Entry64Num; Idx++) {
	    if (*(UINT32 *)(UINTN)(Entry64[Idx]) == EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
	      Fadt = (EFI_ACPI_3_0_FIXED_ACPI_DESCRIPTION_TABLE *)(UINTN)(Entry64[Idx]);  
	      if (pPmCtrlReg)
	      	*pPmCtrlReg = Fadt->Pm1aCntBlk;	      	
	      DEBUG ((EFI_D_ERROR, "PmCtrl Reg 0x%x\n", Fadt->Pm1aCntBlk));
	      	
	      if (pPmTimerReg)   
	      	*pPmTimerReg = Fadt->PmTmrBlk; 
	      DEBUG ((EFI_D_ERROR, "PmTimer Reg 0x%x\n", Fadt->PmTmrBlk));
	      	
	      if (pResetReg)   
	      	*pResetReg = (UINTN)Fadt->ResetReg.Address; 
	      DEBUG ((EFI_D_ERROR, "Reset Reg 0x%lx\n", Fadt->ResetReg.Address));
	      	
	      if (pResetValue)   
	      	*pResetValue = Fadt->ResetValue;
	      DEBUG ((EFI_D_ERROR, "Reset Value 0x%x\n", Fadt->ResetValue));
	      	      	         
	      return RETURN_SUCCESS;        
	    }
	  }
	}	
	
	return RETURN_NOT_FOUND;
}

/**
  Find the serial port information

  @param  pRegBase           Pointer to the base address of serial port registers
  @param  pRegAccessType     Pointer to the access type of serial port registers
  @param  pBaudrate          Pointer to the serial port baudrate

  @retval RETURN_SUCCESS     Successfully find the serial port information.
  @retval RETURN_NOT_FOUND   Failed to find the serial port information .

**/
RETURN_STATUS
CbParseSerialInfo (
  IN UINT32*     pRegBase,
  IN UINT32*     pRegAccessType,
  IN UINT32*     pBaudrate
  )
{
	struct cb_serial*   CbSerial;
	
	CbSerial = FindCbTag (0, CB_TAG_SERIAL);
	if (!CbSerial)
		CbSerial = FindCbTag ((VOID *)(UINTN)PcdGet32 (PcdCbHeaderPointer), CB_TAG_SERIAL);
	
	if (!CbSerial)
		return RETURN_NOT_FOUND;
		
	if (pRegBase)
		*pRegBase = CbSerial->baseaddr;
		
	if (pRegAccessType)
		*pRegAccessType = CbSerial->type;
		
	if (pBaudrate)
		*pBaudrate = CbSerial->baud;
			
	return RETURN_SUCCESS;
}

/**
  Search for the coreboot table header

  @param  Level              Level of the search depth
  @param  HeaderPtr          Pointer to the pointer of coreboot table header

  @retval RETURN_SUCCESS     Successfully find the coreboot table header .
  @retval RETURN_NOT_FOUND   Failed to find the coreboot table header .

**/
RETURN_STATUS
CbParseGetCbHeader (
  IN UINTN  Level,
  IN VOID** HeaderPtr
  )
{
	UINTN Index;
	VOID* TempPtr;
	
	if (!HeaderPtr)
		return RETURN_NOT_FOUND;
	
	TempPtr = NULL;	
	for (Index = 0; Index < Level; Index++) {
		TempPtr = FindCbTag (TempPtr, CB_TAG_FORWARD);
		if (!TempPtr)
			break;		
	}
	
	if ((Index >= Level) && (TempPtr != NULL)) {
		*HeaderPtr = TempPtr;
		return RETURN_SUCCESS;
	}
	
	return RETURN_NOT_FOUND;
}

/**
  Find the video frame buffer information

  @param  pFbInfo            Pointer to the FRAME_BUFFER_INFO structure

  @retval RETURN_SUCCESS     Successfully find the video frame buffer information.
  @retval RETURN_NOT_FOUND   Failed to find the video frame buffer information .

**/
RETURN_STATUS
CbParseFbInfo (
  IN FRAME_BUFFER_INFO*     pFbInfo
  )
{
	struct cb_framebuffer*   CbFbRec;
	
	if (!pFbInfo)
		return RETURN_INVALID_PARAMETER;
	
	CbFbRec = FindCbTag (0, CB_TAG_FRAMEBUFFER);
	if (!CbFbRec)
		CbFbRec = FindCbTag ((VOID *)(UINTN)PcdGet32 (PcdCbHeaderPointer), CB_TAG_FRAMEBUFFER);
	
	if (!CbFbRec)
		return RETURN_NOT_FOUND;
		
  DEBUG ((EFI_D_ERROR, "Found coreboot video frame buffer information\n"));
  DEBUG ((EFI_D_ERROR, "physical_address: 0x%lx\n", CbFbRec->physical_address));
  DEBUG ((EFI_D_ERROR, "x_resolution: 0x%x\n", CbFbRec->x_resolution));
  DEBUG ((EFI_D_ERROR, "y_resolution: 0x%x\n", CbFbRec->y_resolution));
  DEBUG ((EFI_D_ERROR, "bits_per_pixel: 0x%x\n", CbFbRec->bits_per_pixel));
  DEBUG ((EFI_D_ERROR, "bytes_per_line: 0x%x\n", CbFbRec->bytes_per_line));
  
  DEBUG ((EFI_D_ERROR, "red_mask_size: 0x%x\n", CbFbRec->red_mask_size));
  DEBUG ((EFI_D_ERROR, "red_mask_pos: 0x%x\n", CbFbRec->red_mask_pos));
  DEBUG ((EFI_D_ERROR, "green_mask_size: 0x%x\n", CbFbRec->green_mask_size));
  DEBUG ((EFI_D_ERROR, "green_mask_pos: 0x%x\n", CbFbRec->green_mask_pos));
  DEBUG ((EFI_D_ERROR, "blue_mask_size: 0x%x\n", CbFbRec->blue_mask_size));
  DEBUG ((EFI_D_ERROR, "blue_mask_pos: 0x%x\n", CbFbRec->blue_mask_pos));
  DEBUG ((EFI_D_ERROR, "reserved_mask_size: 0x%x\n", CbFbRec->reserved_mask_size));
  DEBUG ((EFI_D_ERROR, "reserved_mask_pos: 0x%x\n", CbFbRec->reserved_mask_pos));
	
  pFbInfo->LinearFrameBuffer    = CbFbRec->physical_address;  
  pFbInfo->HorizontalResolution = CbFbRec->x_resolution;
  pFbInfo->VerticalResolution   = CbFbRec->y_resolution;
  pFbInfo->BitsPerPixel         = CbFbRec->bits_per_pixel;
  pFbInfo->BytesPerScanLine     = (UINT16)CbFbRec->bytes_per_line;
  pFbInfo->Red.Mask             = (1 << CbFbRec->red_mask_size) - 1;
  pFbInfo->Red.Position         = CbFbRec->red_mask_pos;
  pFbInfo->Green.Mask           = (1 << CbFbRec->green_mask_size) - 1;
  pFbInfo->Green.Position       = CbFbRec->green_mask_pos;
  pFbInfo->Blue.Mask            = (1 << CbFbRec->blue_mask_size) - 1;
  pFbInfo->Blue.Position        = CbFbRec->blue_mask_pos;
  pFbInfo->Reserved.Mask        = (1 << CbFbRec->reserved_mask_size) - 1;
  pFbInfo->Reserved.Position    = CbFbRec->reserved_mask_pos;  
  			
	return RETURN_SUCCESS;
}


