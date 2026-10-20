Map-time TPH consumer prototype
==============================

This local prototype builds on Alistair's ``dma_mapping_types`` tree. It tests
the mlx5 ordering requested in the October 15 discussion: map before selecting
TPH and allocating a memory key. It does not implement the proposed general
importer/exporter converter framework.

Attachment-scoped interface
--------------------------

``dma_buf_get_pci_tph(attach, sgt, extended, &tag, &ph)`` queries an optional
exporter callback under ``dma_resv``. The SGT (scatter-gather table) must be the
live result of mapping that attachment. Core removes its debug wrapper before
calling the exporter. Only an eligible raw steering tag and two-bit processing
hint are returned; exporter physical addresses and the provider stay private.

Success describes direct peer access using untranslated Requests and strict
Completions. The caller must use that transaction mode. ``-EOPNOTSUPP`` means
that no eligible hint is available and allows registration without exporter
TPH. Other errors abort initial registration. Normal tag zero is valid, normal
tags are eight bits, and extended tags are sixteen bits.

VFIO obtains eligibility through ``dma_buf_phys_vec_is_direct()`` on its
original SGT. The physical mapping helper retains the address-selection result
it used to construct that mapping. This avoids a second topology query and a
module reference to the unexported ``pci_p2pdma_map_type()`` function. The small
exported helper is for exporters using ``dma_buf_phys_vec_to_sgt()`` only.

This uses the base tree's cached PCI routing decision. ACS controls must stay
stable; no fresh fabric check or support for live ACS changes is claimed.
Host-bridge mappings return no exporter hint. Untagged operation retains the
base mapping behavior and its topology limitations.

mlx5 registration and invalidation
---------------------------------

1. Attach using the existing SGT mapping type. Pinned registration retains its
   existing pin and revoke lifecycle.
2. Lock ``dma_resv`` and map pages without an MR or mkey. If requester TPH is
   enabled and no explicit DMA handle was supplied, query the live mapping's
   ST/PH in the requester's enabled normal or extended namespace.
3. Unlock ``dma_resv`` before steering-table and disabled mkey allocation.
   These allocations may reclaim memory and invalidate the early mapping.
4. Publish the initialized MR pointer under ``dma_resv``. Lock again for the
   existing synchronous initialization, remap if needed, and re-query any saved
   exporter tuple before installing PAS entries or enabling the mkey.
5. On later invalidation, the existing callback zaps the MR translation and
   unmaps the SGT. Before an MR exists, it still unmaps the early SGT. A tagged
   remap must match the saved ST, PH, and namespace eligibility before PAS is
   installed again.

Exporters implementing this narrow TPH callback use ATS-disabled mlx5 mkeys,
including when hints are unavailable, requester TPH is disabled, or an explicit
DMA handle takes precedence. This keeps Requests untranslated. Other exporters
retain the base mlx5 ATS behavior. This is not route-based ATS selection.

Explicit DMA handles retain precedence over automatic exporter hints. The MR
holds its own steering-table reference until successful revoke or destruction;
allocation failures release their references. An initially untagged MR remains
untagged across remaps.

Limits and verification
-----------------------

A tagged remap with changed ST/PH or lost eligibility returns ``-EFAULT`` and
unmaps the new SGT. It does not replace a hardware key while preserving its
userspace identity. That recovery mechanism remains separate work. The base
invalidation callback also ignores UMR ZAP failures; DMA quiescence on such a
failure remains unresolved. This prototype does not establish safe recovery
from that failure or from live routing changes.

The base VFIO PAL interface remains available for existing iommufd consumers;
the new TPH query accepts SGT mappings only. No local PHYS/PCIe converter or
general ATS policy is added.

The ``dma_buf_tph`` KUnit suite exercises attachment ownership, hint snapshots,
debug SGT wrapping, early invalidation, remapping, unsupported hints and invalid
values using a fake exporter. It does not test PCI hardware, mlx5 firmware,
PAS installation, or ZAP failures. Hardware verification is still required.
