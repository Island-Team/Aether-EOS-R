# Generic output data selection

[中文](../../../../docs_cn/isos/interfaces/ui/selection_cn.md) · [Presentation HTTP](../../develop/http/presentations.md) · [Release types](../packages/release.md)

## Overview

The release `DataType` and registered output port are the type authority. Release validation checks the ID/version suffix; an owner registration must prove the port/type binding before descriptors reach the selector. IsOS selects public presentation metadata for an applied output; the producer owns payload bytes, encoding, endpoint authorization, and delivery. A MIME label or UI method alone cannot authorize decoding. No second presentation manifest is introduced.

## Type authority

[Node data types](../nodes/data-types.md) are the single field authority for `id`, `version`, `purpose`, `media_type`, `codec`, `schema`, and `max_payload_bytes`. A registered output port binds that type. The owner descriptor then binds node path, port, representation, delivery, opaque endpoint ID, run ID, and graph revision; see [presentation descriptor](presentation.md) for its fields. IsOS selects metadata only and never takes payload bytes or endpoint authorization.

## Representation families

JSON records and text use `json` and `text`; JSON time series marked `time_series` use `chart`; JPEG/PNG use `image`; bounded RGB/RGBA/depth planes use `canvas`; JSON detections marked `detections` use `overlay`; Opus or PCM audio uses `audio`. Depth requires a unit; producer frame contracts must also define invalid-value semantics. Detections need coordinate space and image association, and audio needs sample rate and channel semantics. These payload fields belong to the producer contract, so IsOS does not invent them from a type name.

## Availability

The current IsOS route performs input and run/revision checks, then returns 503 because no selected owner publishes verified public descriptors. See [presentations](../../develop/http/presentations.md) for its exact request and current error. Stop, Restart, and Apply invalidate an earlier selection.
