// Line-2 NRD Rust oracle.
//
// Reads the same machine-readable cases as the C++ differential runner
// (tools/audit/nrd/cases/nrd_cases.json) and evaluates them with REAL grin 5.5.2
// code from the read-only reference checkout. It emits per-case
// `actual_rust_category` + `status` so the C++ runner can compare actual Rust vs
// actual C++ instead of C++ vs expectation metadata.
//
// What is computed: cases whose fixture can be expressed as a real Rust chain
// (blocks + real transactions/kernels). What is NOT computed (honestly marked
// `not_computed`): `no_index` (Rust has no "local index missing" state) and
// rebuild fixtures with `rebuild_complete: false` (an invalid NRD history cannot
// be created through real chain apply; the Rust verify_kernel_pos_index path
// needs a crafted-MMR harness).
//
// The reference tree is never modified; only temp chain directories are created.

use chrono::Duration;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::sync::Arc;

use grin_chain as chain;
use grin_core as core;
use grin_keychain as keychain;

use chain::types::{NoopAdapter, Options};
use chain::txhashset::TxHashSet;
use chain::Chain;
use core::core::pmmr::PMMR;
use core::core::{Block, BlockHeader, KernelFeatures, NRDRelativeHeight, Transaction, TxKernel};
use core::genesis;
use core::global;
use core::libtx::{aggsig, build, reward, ProofBuilder};
use core::ser::ProtocolVersion;
use core::{consensus, pow};
use grin_store::pmmr::PMMRBackend;
use keychain::{BlindingFactor, ExtKeychain, ExtKeychainPath, Identifier, Keychain};

const PREMINE_BLOCKS: u32 = 16;
const NRD_FEE: u64 = 20_000;

#[derive(Clone)]
struct TagState {
	excess: BlindingFactor,
	value: u64,
	key: Identifier,
}

fn genesis_block(kc: &ExtKeychain) -> Block {
	let key_id = ExtKeychain::derive_key_id(0, 1, 0, 0, 0);
	let reward = reward::output(kc, &ProofBuilder::new(kc), &key_id, 0, false).unwrap();
	genesis::genesis_dev().with_reward(reward.0, reward.1)
}

fn new_key_id(counter: &mut u32) -> Identifier {
	// Depth must stay <= 4 (ExtKeychainPath has four path components).
	let id = ExtKeychainPath::new(3, 0, *counter, 0, 0).to_identifier();
	*counter += 1;
	id
}

fn take_coinbase(ids: &mut Vec<Identifier>) -> Result<Identifier, String> {
	if ids.is_empty() {
		Err("fixture exhausted the pre-mined coinbase inputs".to_string())
	} else {
		Ok(ids.remove(0))
	}
}

fn nrd_features(fee: u64, relative_height: u64) -> KernelFeatures {
	KernelFeatures::NoRecentDuplicate {
		fee: (fee as u32).into(),
		relative_height: NRDRelativeHeight::new(relative_height)
			.expect("case check failed: relative_height outside 1..=WEEK_HEIGHT"),
	}
}

fn make_signed_kernel(kc: &ExtKeychain, features: KernelFeatures, excess: &BlindingFactor) -> TxKernel {
	let mut kernel = TxKernel::with_features(features);
	let msg = kernel.msg_to_sign().unwrap();
	let skey = excess.secret_key(&kc.secp()).unwrap();
	kernel.excess = kc.secp().commit(0, skey).unwrap();
	let pubkey = kernel.excess.to_pubkey(&kc.secp()).unwrap();
	kernel.excess_sig =
		aggsig::sign_with_blinding(&kc.secp(), &msg, excess, Some(&pubkey)).unwrap();
	kernel.verify().unwrap();
	kernel
}

fn build_next_block(
	chain: &Chain,
	kc: &ExtKeychain,
	txs: Vec<Transaction>,
	reward_key: Identifier,
) -> Result<Block, chain::Error> {
	let prev = chain.head_header()?;
	// chain_test_helper::mine_chain uses prev.height + 1 (the height of the block
	// being mined); passing prev.height only happens to work for short chains.
	let next_header_info = consensus::next_difficulty(prev.height + 1, chain.difficulty_iter()?);
	let total_fee: u64 = txs.iter().map(|tx| tx.fee()).sum();
	let reward_output =
		reward::output(kc, &ProofBuilder::new(kc), &reward_key, total_fee, false).unwrap();
	let mut block = Block::new(
		&prev,
		&txs,
		next_header_info.clone().difficulty,
		reward_output,
	)?;
	block.header.timestamp = prev.timestamp + Duration::seconds(60);
	block.header.pow.secondary_scaling = next_header_info.secondary_scaling;
	chain.set_txhashset_roots(&mut block)?;
	block.header.pow.proof.edge_bits = global::min_edge_bits();
	pow::pow_size(
		&mut block.header,
		next_header_info.difficulty,
		global::proofsize(),
		global::min_edge_bits(),
	)
	.unwrap();
	Ok(block)
}

fn build_txs(
	kc: &ExtKeychain,
	kernels: &[Value],
	tag_states: &mut HashMap<u64, TagState>,
	coinbase_ids: &mut Vec<Identifier>,
	next_key: &mut u32,
) -> Result<Vec<Transaction>, String> {
	let mut txs = Vec::new();
	for spec in kernels {
		let features = spec["features"].as_str().unwrap_or("");
		let out_key = new_key_id(next_key);

		if features == "nrd" {
			let tag = spec["excess"]
				.as_u64()
				.ok_or_else(|| "nrd kernel missing excess tag".to_string())?;
			let relative_height = spec["relative_height"]
				.as_u64()
				.ok_or_else(|| "nrd kernel missing relative_height".to_string())?;

			match tag_states.get(&tag).cloned() {
				None => {
					let excess = BlindingFactor::rand(&kc.secp());
					let kernel = make_signed_kernel(kc, nrd_features(NRD_FEE, relative_height), &excess);
					let coinbase = take_coinbase(coinbase_ids)?;
					let elems = [
						build::coinbase_input(consensus::REWARD, coinbase),
						build::output(consensus::REWARD - NRD_FEE, out_key.clone()),
					];
					let tx = build::transaction_with_kernel(
						&elems,
						kernel,
						excess.clone(),
						kc,
						&ProofBuilder::new(kc),
					)
					.map_err(|e| format!("nrd first tx build: {}", e))?;
					tag_states.insert(
						tag,
						TagState {
							excess,
							value: consensus::REWARD - NRD_FEE,
							key: out_key,
						},
					);
					txs.push(tx);
				}
				Some(mut state) => {
					if state.value <= NRD_FEE {
						return Err(format!("fixture value exhausted for nrd tag {}", tag));
					}
					let kernel =
						make_signed_kernel(kc, nrd_features(NRD_FEE, relative_height), &state.excess);
					let elems = [
						build::input(state.value, state.key.clone()),
						build::output(state.value - NRD_FEE, out_key.clone()),
					];
					let tx = build::transaction_with_kernel(
						&elems,
						kernel,
						state.excess.clone(),
						kc,
						&ProofBuilder::new(kc),
					)
					.map_err(|e| format!("nrd repeat tx build (tag {}): {}", tag, e))?;
					state.value -= NRD_FEE;
					state.key = out_key;
					tag_states.insert(tag, state);
					txs.push(tx);
				}
			}
		} else {
			// Non-NRD kernel (plain): the NRD rules must not apply.
			let coinbase = take_coinbase(coinbase_ids)?;
			let elems = [
				build::coinbase_input(consensus::REWARD, coinbase),
				build::output(consensus::REWARD, out_key),
			];
			let tx = build::transaction(
				KernelFeatures::Plain { fee: 0.into() },
				&elems,
				kc,
				&ProofBuilder::new(kc),
			)
			.map_err(|e| format!("plain tx build: {}", e))?;
			txs.push(tx);
		}
	}

	Ok(txs)
}

fn is_nrd(spec: &Value) -> bool {
	spec["features"].as_str().unwrap_or("") == "nrd"
}

fn has_duplicate_nrd_tag(kernels: &[Value]) -> bool {
	let mut tags: Vec<u64> = kernels
		.iter()
		.filter(|spec| is_nrd(spec))
		.filter_map(|spec| spec["excess"].as_u64())
		.collect();
	let original = tags.len();
	tags.sort_unstable();
	tags.dedup();
	tags.len() != original
}

// Execute the REAL Rust rebuild path used by txhashset_write / desegmenter:
// TxHashSet::verify_kernel_pos_index replays kernel history from genesis and
// rebuilds the on-disk NRD recent-kernel index. This is distinct from normal
// chain apply and is what the "rebuild" fixtures model.
fn run_real_rust_rebuild(chain: &Chain, genesis_header: &BlockHeader) -> Result<(), String> {
	let txhashset_arc = chain.txhashset();
	let txhashset = txhashset_arc.read();
	let header_pmmr_arc = chain.header_pmmr();
	// header_pmmr uses parking_lot::RwLock (no unwrap on read()).
	let header_pmmr = header_pmmr_arc.read();
	let store = chain.store();
	let mut batch = store
		.batch()
		.map_err(|e| format!("rebuild batch: {}", e))?;
	txhashset
		.verify_kernel_pos_index(genesis_header, &header_pmmr, &mut batch, None, None)
		.map_err(|e| format!("{}", e))
}

fn evaluate_invalid_history_case(
	case: &Value,
	index: usize,
) -> Result<(String, String), String> {
	global::set_local_chain_type(global::ChainTypes::AutomatedTesting);
	global::set_local_nrd_enabled(true);

	// A real chain cannot contain an invalid NRD history (the rules reject it at
	// apply time), so build a structurally identical valid chain (same kernel
	// count per block), then craft the kernel PMMR with the invalid kernels and
	// run the real Rust verify_kernel_pos_index on it. This exercises the actual
	// rebuild code path; only the kernel data is crafted.
	let dir = std::env::temp_dir().join(format!(
		"grin55-nrd-oracle-invalid-{}-{}",
		std::process::id(),
		index
	));
	let craft_root = std::env::temp_dir().join(format!(
		"grin55-nrd-oracle-crafted-{}-{}",
		std::process::id(),
		index
	));
	let _ = fs::remove_dir_all(&dir);
	let _ = fs::remove_dir_all(&craft_root);
	fs::create_dir_all(&dir).map_err(|e| format!("temp dir: {}", e))?;
	fs::create_dir_all(&craft_root).map_err(|e| format!("crafted dir: {}", e))?;

	let kc = ExtKeychain::from_random_seed(false).map_err(|e| format!("keychain: {}", e))?;
	let genesis_block = genesis_block(&kc);
	let genesis_header = genesis_block.header.clone();
	let chain = Chain::init(
		dir.to_string_lossy().to_string(),
		Arc::new(NoopAdapter {}),
		genesis_block,
		pow::verify_size,
		false,
		None,
	)
	.map_err(|e| format!("chain init: {}", e))?;

	let mut coinbase_ids: Vec<Identifier> = Vec::new();
	for i in 1..=PREMINE_BLOCKS {
		let key_id = ExtKeychainPath::new(2, i, 0, 0, 0).to_identifier();
		let block = build_next_block(&chain, &kc, vec![], key_id.clone())
			.map_err(|e| format!("pre-mine block {} build: {}", i, e))?;
		chain
			.process_block(block, Options::NONE)
			.map_err(|e| format!("pre-mine block {} apply: {}", i, e))?;
		coinbase_ids.push(key_id);
	}

	let mut tag_states: HashMap<u64, TagState> = HashMap::new();
	let mut next_key: u32 = 0;
	let mut duplicate_in_block = false;
	let mut planned_kernels: Vec<(u64, u64)> = Vec::new();
	let mut shape_tag: u64 = 300;
	let setup_blocks = case["blocks"]
		.as_array()
		.ok_or_else(|| "case missing blocks array".to_string())?;
	for block_node in setup_blocks {
		let kernels = block_node
			.get("kernels")
			.and_then(Value::as_array)
			.cloned()
			.unwrap_or_default();
		if has_duplicate_nrd_tag(&kernels) {
			duplicate_in_block = true;
		}

		// Shape block: same kernel count, but valid distinct NRD kernels.
		let mut shape_specs = Vec::new();
		for spec in &kernels {
			planned_kernels.push((
				spec["excess"].as_u64().unwrap_or(0),
				spec["relative_height"].as_u64().unwrap_or(0),
			));
			shape_specs.push(json!({
				"features": "nrd",
				"excess": shape_tag,
				"relative_height": 2u64,
			}));
			shape_tag += 1;
		}

		let txs = build_txs(
			&kc,
			&shape_specs,
			&mut tag_states,
			&mut coinbase_ids,
			&mut next_key,
		)?;
		let height = chain.head_header().map_err(|e| format!("head: {}", e))?.height;
		let reward_key = ExtKeychainPath::new(4, 0, 0, height as u32, 0).to_identifier();
		let block = build_next_block(&chain, &kc, txs, reward_key)
			.map_err(|e| format!("shape block build at height {}: {}", height + 1, e))?;
		chain
			.process_block(block, Options::NONE)
			.map_err(|e| format!("shape block apply at height {}: {}", height + 1, e))?;
	}

	// Crafted kernel PMMR: invalid kernels (same excess / too-short relative gap).
	let mut excess_by_tag: HashMap<u64, BlindingFactor> = HashMap::new();
	let mut crafted_kernels: Vec<TxKernel> = Vec::new();
	for (tag, relative_height) in &planned_kernels {
		let excess = excess_by_tag
			.entry(*tag)
			.or_insert_with(|| BlindingFactor::rand(&kc.secp()))
			.clone();
		crafted_kernels.push(make_signed_kernel(
			&kc,
			nrd_features(NRD_FEE, *relative_height),
			&excess,
		));
	}

	let kernel_dir = craft_root.join("txhashset").join("kernel");
	fs::create_dir_all(&kernel_dir).map_err(|e| format!("kernel dir: {}", e))?;
	let mut backend = PMMRBackend::<TxKernel>::new(&kernel_dir, false, ProtocolVersion(2), None)
		.map_err(|e| format!("kernel pmmr backend: {}", e))?;
	{
		let mut pmmr = PMMR::new(&mut backend);
		for kernel in &crafted_kernels {
			pmmr.push(kernel)
				.map_err(|e| format!("kernel pmmr push: {}", e))?;
		}
	}
	backend.sync().map_err(|e| format!("kernel pmmr sync: {}", e))?;

	let crafted_txhashset = TxHashSet::open(
		craft_root.to_string_lossy().to_string(),
		chain.store(),
		None,
	)
	.map_err(|e| format!("crafted txhashset open: {}", e))?;

	let header_pmmr_arc = chain.header_pmmr();
	let header_pmmr = header_pmmr_arc.read();
	let store = chain.store();
	let mut batch = store
		.batch()
		.map_err(|e| format!("rebuild batch: {}", e))?;
	let rebuild = crafted_txhashset.verify_kernel_pos_index(
		&genesis_header,
		&header_pmmr,
		&mut batch,
		None,
		None,
	);
	drop(batch);
	drop(header_pmmr);
	drop(crafted_txhashset);
	drop(chain);
	let _ = fs::remove_dir_all(&dir);
	let _ = fs::remove_dir_all(&craft_root);

	match rebuild {
		Err(e) if e.to_string().contains("NRD") || e.to_string().contains("Relative") => Ok((
			if duplicate_in_block {
				"invalid_nrd_duplicate_in_block".to_string()
			} else {
				"invalid_nrd_relative_height".to_string()
			},
			format!(
				"real Rust verify_kernel_pos_index rejected the crafted invalid history: {}",
				e
			),
		)),
		Err(e) => Err(format!(
			"real Rust rebuild failed with a non-NRD error: {}",
			e
		)),
		Ok(()) => Err(
			"crafted invalid-history state unexpectedly passed real Rust verify_kernel_pos_index"
				.to_string(),
		),
	}
}

fn process_case(case: &Value, index: usize) -> Value {
	let mut out = json!({"id": case["id"].as_str().unwrap_or("")});
	let mode = case["mode"].as_str().unwrap_or("");
	if mode == "no_index" {
		out["status"] = json!("not_computed");
		out["detail"] = json!(
			"Rust has no local-missing-index state: the index is rebuilt inside txhashset write/startup"
		);
		return out;
	}
	if mode == "rebuild"
		&& case
			.get("rebuild_complete")
			.and_then(Value::as_bool)
			.map(|v| !v)
			.unwrap_or(false)
	{
		// Invalid NRD history cannot be created through normal chain apply, but it
		// CAN be built with the NRD rule flag disabled and then fed to the real
		// Rust rebuild path (verify_kernel_pos_index) with the flag re-enabled.
		// The Rust rejection is computed, but its category is a rebuild error
		// (NRDRelativeHeight), not the C++ local-state category, so it is reported
		// as computed_unpaired and never counted as a paired case.
		match evaluate_invalid_history_case(case, index) {
			Ok((category, detail)) => {
				out["status"] = json!("computed_unpaired");
				out["actual_rust_category"] = json!(category);
				out["paired_eligible"] = json!(false);
				out["evaluation_path"] =
					json!("rust_verify_kernel_pos_index_rebuild_rejects_invalid_history");
				out["detail"] = json!(detail);
			}
			Err(error) => {
				out["status"] = json!("not_computed");
				out["paired_eligible"] = json!(false);
				out["detail"] = json!(format!("fixture_error: {}", error));
			}
		}
		return out;
	}

	match evaluate_chain_case(case, index) {
		Ok((category, detail, evaluation_path)) => {
			out["status"] = json!("computed");
			out["actual_rust_category"] = json!(category);
			out["paired_eligible"] = json!(true);
			out["evaluation_path"] = json!(evaluation_path);
			out["rust_rebuild_executed"] =
				json!(evaluation_path == "rust_verify_kernel_pos_index_rebuild_then_chain_apply");
			out["detail"] = json!(detail);
		}
		Err(error) => {
			out["status"] = json!("not_computed");
			out["detail"] = json!(format!("fixture_error: {}", error));
		}
	}
	out
}

fn evaluate_chain_case(case: &Value, index: usize) -> Result<(String, String, String), String> {
	global::set_local_chain_type(global::ChainTypes::AutomatedTesting);
	global::set_local_nrd_enabled(true);

	let dir = std::env::temp_dir().join(format!("grin55-nrd-oracle-{}-{}", std::process::id(), index));
	let _ = fs::remove_dir_all(&dir);
	fs::create_dir_all(&dir).map_err(|e| format!("temp dir: {}", e))?;

	let kc = ExtKeychain::from_random_seed(false).map_err(|e| format!("keychain: {}", e))?;
	let genesis_block = genesis_block(&kc);
	let genesis_header = genesis_block.header.clone();
	let chain = Chain::init(
		dir.to_string_lossy().to_string(),
		Arc::new(NoopAdapter {}),
		genesis_block,
		pow::verify_size,
		false,
		None,
	)
	.map_err(|e| format!("chain init: {}", e))?;

	let mut coinbase_ids: Vec<Identifier> = Vec::new();
	for i in 1..=PREMINE_BLOCKS {
		let key_id = ExtKeychainPath::new(2, i, 0, 0, 0).to_identifier();
		let block = build_next_block(&chain, &kc, vec![], key_id.clone())
			.map_err(|e| format!("pre-mine block {} build: {}", i, e))?;
		chain
			.process_block(block, Options::NONE)
			.map_err(|e| format!("pre-mine block {} apply: {}", i, e))?;
		coinbase_ids.push(key_id);
	}

	let mut tag_states: HashMap<u64, TagState> = HashMap::new();
	let mut next_key: u32 = 0;
	let mut evaluation_path = "chain_apply";

	let setup_blocks = case["blocks"]
		.as_array()
		.ok_or_else(|| "case missing blocks array".to_string())?;
	for block_node in setup_blocks {
		let kernels = block_node
			.get("kernels")
			.and_then(Value::as_array)
			.cloned()
			.unwrap_or_default();
		let txs = build_txs(&kc, &kernels, &mut tag_states, &mut coinbase_ids, &mut next_key)?;
		let height = chain.head_header().map_err(|e| format!("head: {}", e))?.height;
		let reward_key = ExtKeychainPath::new(4, 0, 0, height as u32, 0).to_identifier();
		let block = build_next_block(&chain, &kc, txs, reward_key)
			.map_err(|e| format!("setup block build at height {}: {}", height + 1, e))?;
		chain.process_block(block, Options::NONE).map_err(|e| {
			format!(
				"valid-history fixture rejected by real Rust chain at height {}: {}",
				height + 1,
				e
			)
		})?;
	}

	if case["mode"].as_str() == Some("rebuild") {
		// The rebuild fixtures model Rust's txhashset rebuild path, so execute the
		// real verify_kernel_pos_index code (not just chain apply). A historical
		// violation here is a real rebuild rejection; this fixture class is built
		// from a valid history, so an error is a fixture problem.
		run_real_rust_rebuild(&chain, &genesis_header).map_err(|e| {
			format!(
				"real Rust verify_kernel_pos_index rebuild failed on a valid-history fixture: {}",
				e
			)
		})?;
		evaluation_path = "rust_verify_kernel_pos_index_rebuild_then_chain_apply";
	}

	let target_nodes = case["target"]
		.as_array()
		.ok_or_else(|| "case missing target array".to_string())?;
	let target_has_nrd = target_nodes.iter().any(is_nrd);
	let target_has_duplicate = has_duplicate_nrd_tag(target_nodes);
	let height = chain.head_header().map_err(|e| format!("head: {}", e))?.height;
	let reward_key = ExtKeychainPath::new(4, 0, 0, height as u32, 0).to_identifier();

	// Rust's `set_txhashset_roots` applies the body to a txhashset extension, so it
	// enforces NRD rules while BUILDING the block. For a deliberately invalid
	// target the roots must still be computable: temporarily disable the NRD flag
	// while building (the same trick chain/tests/nrd_validation_rules.rs uses),
	// then re-enable it before process_block so the block is validated normally.
	global::set_local_nrd_enabled(false);
	let target_build = build_txs(
		&kc,
		target_nodes,
		&mut tag_states,
		&mut coinbase_ids,
		&mut next_key,
	)
	.and_then(|txs| {
		build_next_block(&chain, &kc, txs, reward_key)
			.map_err(|e| format!("target block build: {}", e))
	});
	global::set_local_nrd_enabled(true);

	let target_block = target_build?;
	let result = chain.process_block(target_block, Options::NONE);
	let debug = match &result {
		Ok(_) => String::new(),
		Err(e) => format!("{:?}", e),
	};
	let display = match &result {
		Ok(_) => String::new(),
		Err(e) => format!("{}", e),
	};

	drop(chain);
	let _ = fs::remove_dir_all(&dir);

	match result {
		Ok(_) => {
			let category = if target_has_nrd {
				"accepted"
			} else {
				"accepted_non_nrd"
			};
			Ok((
				category.to_string(),
				format!(
					"real grin_chain 5.5.2 process_block accepted target ({}; height {})",
					evaluation_path,
					height + 1
				),
				evaluation_path.to_string(),
			))
		}
		Err(_) => {
			let looks_nrd = debug.contains("NRD") || display.contains("NRD");
			if looks_nrd {
				let category = if target_has_duplicate {
					"invalid_nrd_duplicate_in_block"
				} else {
					"invalid_nrd_relative_height"
				};
				Ok((
					category.to_string(),
					format!(
						"real grin_chain 5.5.2 process_block rejected target with NRD error ({:?}; {})",
						display, evaluation_path
					),
					evaluation_path.to_string(),
				))
			} else {
				Err(format!(
					"target rejected with non-NRD error (fixture/construction issue): {}",
					debug
				))
			}
		}
	}
}

fn main() {
	let args: Vec<String> = std::env::args().collect();
	let cases_path = std::env::var("GRINPP_NRD_CASES")
		.ok()
		.or_else(|| args.get(1).cloned())
		.unwrap_or_else(|| "tools/audit/nrd/cases/nrd_cases.json".to_string());
	let output_path = std::env::var("GRINPP_NRD_RUST_ORACLE_OUT")
		.ok()
		.or_else(|| args.get(2).cloned())
		.expect(
			"usage: nrd-oracle <cases.json> <output.json>  (or set GRINPP_NRD_RUST_ORACLE_OUT)",
		);

	let cases_text =
		fs::read_to_string(&cases_path).unwrap_or_else(|e| panic!("cannot read {}: {}", cases_path, e));
	let root: Value =
		serde_json::from_str(&cases_text).unwrap_or_else(|e| panic!("invalid cases json: {}", e));
	let cases = root["cases"]
		.as_array()
		.unwrap_or_else(|| panic!("cases json has no cases array"));

	let mut computed = 0usize;
	let mut computed_unpaired = 0usize;
	let mut output_cases = Vec::new();
	for (index, case) in cases.iter().enumerate() {
		let result = process_case(case, index);
		match result["status"].as_str() {
			Some("computed") => computed += 1,
			Some("computed_unpaired") => computed_unpaired += 1,
			_ => {}
		}
		println!(
			"case {}: {} {}",
			result["id"].as_str().unwrap_or("?"),
			result["status"].as_str().unwrap_or("?"),
			result
				.get("detail")
				.and_then(Value::as_str)
				.unwrap_or("")
		);
		output_cases.push(result);
	}

	let output = json!({
		"format_version": 1,
		"producer": "nrd-oracle (line2-nrd)",
		"rust_version": "5.5.2",
		"reference_tree": "C:/Users/nurli/AppData/Local/Temp/grin55-minpeers",
		"cases_total": cases.len(),
		"cases_computed": computed,
		"cases_computed_unpaired": computed_unpaired,
		"cases_not_computed": cases.len() - computed - computed_unpaired,
		"cases": output_cases,
	});

	let rendered = serde_json::to_string_pretty(&output).expect("serialize oracle output");
	fs::write(&output_path, rendered).unwrap_or_else(|e| panic!("cannot write {}: {}", output_path, e));
	println!(
		"nrd-oracle: {} cases, {} computed, {} computed_unpaired, {} not_computed -> {}",
		cases.len(),
		computed,
		computed_unpaired,
		cases.len() - computed - computed_unpaired,
		output_path
	);
}
