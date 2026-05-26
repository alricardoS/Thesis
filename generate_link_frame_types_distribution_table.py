#!/usr/bin/env python3
"""
Generate frame type distribution table by link and priority class.

Analyzes phy_rx_pkt_type data to show what types of frames (Data, Control, Management)
are passing through each link for each access class.
"""

import pandas as pd
import sys
from pathlib import Path

def get_frame_category(mac_type):
    """Categorize MAC frame type into Data/Control/Management."""
    mac_type = str(mac_type).upper()
    
    if 'QOSDATA' in mac_type or 'DATA' in mac_type:
        return 'Data'
    elif any(x in mac_type for x in ['RTS', 'CTS', 'ACK', 'BACKREQ', 'BLOCKACK', 'CTL_']):
        return 'Control'
    elif any(x in mac_type for x in ['MGT_', 'BEACON', 'PROBE', 'ASSOC', 'AUTH', 'DEAUTH']):
        return 'Management'
    else:
        return 'Other'


def extract_freq_from_band(freq_band):
    """Extract frequency name from band string like '2.4GHz', '5GHz', '6GHz'."""
    if isinstance(freq_band, str):
        return freq_band.replace('GHz', '').replace(' ', '')
    return str(freq_band)


def generate_link_frame_types_table(outputs_dir, scenario_name, table_output_file):
    """
    Generate frame type distribution table for a given scenario.
    
    Uses:
    - single_multi_sta_phy_rx_pkt_type_*.csv (RX packet type statistics)
    - mlo_multi_sta_queue_occupancy.csv (for AC information)
    """
    
    outputs_path = Path(outputs_dir)
    
    # Find phy_rx_pkt_type CSV (contains frame type info at PHY layer)
    phy_rx_files = list(outputs_path.glob('*_phy_rx_pkt_type_*.csv'))
    if not phy_rx_files:
        print(f"Warning: No PHY RX packet type CSV found in {outputs_dir}")
        return False
    
    phy_rx_file = sorted(phy_rx_files)[-1]  # Get most recent
    queue_occupancy_file = outputs_path / 'mlo_multi_sta_queue_occupancy.csv'
    
    # Load PHY RX data
    try:
        phy_df = pd.read_csv(phy_rx_file)
    except Exception as e:
        print(f"Error loading PHY RX CSV: {e}")
        return False
    
    # Load queue occupancy if available (to get AC distribution)
    queue_df_ap = pd.DataFrame()
    if queue_occupancy_file.exists():
        try:
            queue_df = pd.read_csv(queue_occupancy_file)
            queue_df_ap = queue_df[queue_df['role'] == 'AP'].copy() if 'role' in queue_df.columns else queue_df
        except:
            pass
    
    # Get unique frequency bands from phy_rx data
    if 'freq_band' in phy_df.columns:
        freq_bands = sorted(phy_df['freq_band'].unique())
        freq_pair = '+'.join([extract_freq_from_band(fb) for fb in freq_bands])
    else:
        freq_pair = "unknown"
        freq_bands = []
    
    # Process data
    output_rows = []
    
    # Group by frequency band and frame type
    for freq_band in freq_bands:
        freq_df = phy_df[phy_df['freq_band'] == freq_band].copy() if 'freq_band' in phy_df.columns else phy_df
        
        freq_short = extract_freq_from_band(freq_band)
        
        # Count frames by mac_type (which we'll categorize)
        if 'mac_type' in freq_df.columns and 'count' in freq_df.columns:
            frame_data = {}
            
            for _, row in freq_df.iterrows():
                mac_type = row['mac_type']
                count = int(row.get('count', 0))
                category = get_frame_category(mac_type)
                
                if category not in frame_data:
                    frame_data[category] = 0
                frame_data[category] += count
            
            # Create rows for each frame type
            total_frames = sum(frame_data.values())
            for frame_type in ['Data', 'Control', 'Management', 'Other']:
                count = frame_data.get(frame_type, 0)
                pct = (count / total_frames * 100) if total_frames > 0 else 0
                
                # If we have queue data, try to correlate with ACs
                ac_info = "All"
                if not queue_df_ap.empty and frame_type == 'Data':
                    # Most data frames are likely QoS frames from ACs with traffic
                    acs_with_traffic = queue_df_ap[queue_df_ap['bytes'] > 0]['ac'].unique()
                    if len(acs_with_traffic) > 0:
                        ac_info = ','.join(sorted(acs_with_traffic))
                
                output_rows.append({
                    'Frequency': freq_short,
                    'Frame_Type': frame_type,
                    'Count': count,
                    'Percentage': f'{pct:.2f}%',
                    'Primary_AC': ac_info
                })
    
    # If no detailed data, create basic structure
    if not output_rows:
        print(f"Warning: No detailed frame data in {phy_rx_file}")
        # Create placeholders
        for freq_band in freq_bands:
            freq_short = extract_freq_from_band(freq_band)
            for frame_type in ['Data', 'Control', 'Management']:
                output_rows.append({
                    'Frequency': freq_short,
                    'Frame_Type': frame_type,
                    'Count': 0,
                    'Percentage': '0.00%',
                    'Primary_AC': 'N/A'
                })
    
    # Write output table
    try:
        with open(table_output_file, 'w') as f:
            f.write('=' * 120 + '\n')
            f.write(f'MLO Link Frame Types Distribution - {scenario_name}\n')
            f.write('=' * 120 + '\n\n')
            f.write(f'Scenario: {scenario_name}\n')
            f.write(f'Frequency Pair: {freq_pair}\n')
            f.write(f'Analysis: PHY RX Layer\n\n')
            f.write('Frame Type Distribution by Link:\n')
            f.write('-' * 120 + '\n')
            
            if output_rows:
                output_df = pd.DataFrame(output_rows)
                f.write(output_df.to_string(index=False))
                f.write('\n\n')
            
            f.write('Legend:\n')
            f.write('- Frequency: Radio frequency band\n')
            f.write('- Frame_Type: Category of frames (Data/Control/Management)\n')
            f.write('  * Data: QoS Data frames carrying user traffic\n')
            f.write('  * Control: ACK, BlockAck, RTS/CTS frames\n')
            f.write('  * Management: Beacon, Association, Authentication frames\n')
            f.write('- Count: Number of frames of this type at PHY RX\n')
            f.write('- Percentage: Percentage of total RX frames on this link\n')
            f.write('- Primary_AC: Access Classes observed to use this link (from queue data)\n')
        
        print(f"✓ Generated: {table_output_file}")
        return True
        
    except Exception as e:
        print(f"Error writing table: {e}")
        return False


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_link_frame_types_distribution_table.py <outputs_dir> [scenario_name] [output_file]")
        sys.exit(1)
    
    outputs_dir = sys.argv[1]
    scenario_name = sys.argv[2] if len(sys.argv) > 2 else Path(outputs_dir).parent.name
    output_file = sys.argv[3] if len(sys.argv) > 3 else "link_frame_types_distribution_table.txt"
    
    success = generate_link_frame_types_table(outputs_dir, scenario_name, output_file)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
