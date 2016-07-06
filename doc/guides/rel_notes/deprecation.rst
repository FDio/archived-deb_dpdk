ABI and API Deprecation
=======================

See the :doc:`guidelines document for details of the ABI policy </contributing/versioning>`.
API and ABI deprecation notices are to be posted here.


Deprecation Notices
-------------------

* The log history is deprecated.
  It is voided in 16.07 and will be removed in release 16.11.

* The ethdev hotplug API is going to be moved to EAL with a notification
  mechanism added to crypto and ethdev libraries so that hotplug is now
  available to both of them. This API will be stripped of the device arguments
  so that it only cares about hotplugging.

* Structures embodying pci and vdev devices are going to be reworked to
  integrate new common rte_device / rte_driver objects (see
  http://dpdk.org/ml/archives/dev/2016-January/031390.html).
  ethdev and crypto libraries will then only handle those objects so that they
  do not need to care about the kind of devices that are being used, making it
  easier to add new buses later.

* ABI changes are planned for adding four new flow types. This impacts
  RTE_ETH_FLOW_MAX. The release 2.2 does not contain these ABI changes,
  but release 2.3 will. [postponed]

* The mbuf flags PKT_RX_VLAN_PKT and PKT_RX_QINQ_PKT are deprecated and
  are respectively replaced by PKT_RX_VLAN_STRIPPED and
  PKT_RX_QINQ_STRIPPED, that are better described. The old flags and
  their behavior will be kept in 16.07 and will be removed in 16.11.

* The APIs rte_mempool_count and rte_mempool_free_count are being deprecated
  on the basis that they are confusing to use - free_count actually returns
  the number of allocated entries, not the number of free entries as expected.
  They are being replaced by rte_mempool_avail_count and
  rte_mempool_in_use_count respectively.

* The mempool functions for single/multi producer/consumer are deprecated and
  will be removed in 16.11.
  It is replaced by rte_mempool_generic_get/put functions.
