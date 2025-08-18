

# Quark Serialization Protocol

Quark utilizes a TLV (Type Length Value) protocol. 

## Goals
    1. Zero-copy
    2. Backwards/Forward compatibility
    3. Compact Headers
    4. Flexible Fields
    5. Nested messages

## 1. Overview
The Quark protocol defines a compact binary encoding for primitive types.  
Each value begins with a **Type Tag** (`uint8_t`), followed by type-specific data.

## 2. Type Tags
| Type     | Value |
|----------|-------|
| INT32    | `0x01` |
| FLOAT32  | `0x02` |
| STRING   | `0x03` |

---

## 3. Data Layouts

### 3.1 INT32
[ INT32 | int32_data ]

yaml
Copy code
- `INT32` → type tag (`0x01`)  
- `int32_data` → raw 4-byte little-endian integer  

---

### 3.2 FLOAT32
[ FLOAT32 | float32_data ]

yaml
Copy code
- `FLOAT32` → type tag (`0x02`)  
- `float32_data` → raw 4-byte little-endian float  

---

### 3.3 STRING
[ STRING | Length | string_data ]

markdown
Copy code
- `STRING` → type tag (`0x03`)  
- `Length` → varint (size of `string_data` in bytes)  
- `string_data` → UTF-8 encoded string bytes  

---

## 4. Varint Encoding

- Unsigned integer stored in a variable number of bytes  
- Each byte encodes 7 bits  
- MSB (most significant bit) is the continuation flag:
  - `1` → more bytes follow  
  - `0` → last byte  

### Example
Value: 300
Binary: 0000 0010 1010 1100

Varint: [0xAC, 0x02]
0xAC = 1010 1100 (continuation set, lower 7 bits = 0x2C)
0x02 = 0000 0010 (no continuation, lower 7 bits = 0x02)

yaml
Copy code

---

## 5. Examples

### 5.1 Serialize INT32(42)
[ 0x01 | 0x2A 0x00 0x00 0x00 ]

scss
Copy code

### 5.2 Serialize STRING("hi")
[ 0x03 | 0x02 | 0x68 0x69 ]

markdown
Copy code
- `0x03` = STRING tag  
- `0x02` = varint length (2 bytes)  
- `0x68 0x69` = "hi" in UTF-8  



